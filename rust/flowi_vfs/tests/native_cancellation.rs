//! Cancellation against the real C VFS: dropping a handle, a mapped read or a task has to
//! come back on the calling thread while the operation behind it is still running.
//!
//! The in-crate tests drive a fake call table, which cannot show this - `vfs_close` and the
//! deferred reclamation behind it are the subject, so the coverage has to go through the
//! native library. The process runs with a single job worker and a gate job holds it, which
//! makes an operation queued behind it provably unfinished at the moment the close runs.
//! The gate's own timeout is the safety valve: a close that blocks turns into a failed
//! assertion instead of a hung test run.
//!
//! Bring-up is spelled out against `flowi_core_sys` rather than `flowi_core::init`, because
//! `fl_init` is not among the symbols the shared library this crate links exports.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use flowi_vfs::{Arena, LocalExecutor, Mount, OwnedMount, Poll, Vfs};

/// There is one job worker and one VFS per process, and every test here wants the worker to
/// itself, so they take turns. The flag is whether the first test has brought them up.
static PROCESS: Mutex<bool> = Mutex::new(false);

const FILE_NAME: &str = "payload.bin";
const FILE_BYTES: &[u8] = b"gated";

/// Reserved address space for core's main arena - what C's `arena_new` asks for. Reserved,
/// not committed.
const ARENA_RESERVE: u64 = 2 * 1024 * 1024 * 1024;

/// How long the gate holds the worker before letting go on its own, and the deadline every
/// spin here gives up at. Far longer than any scheduling hiccup, short enough that a
/// regression fails in seconds rather than hanging CI.
const PATIENCE: Duration = Duration::from_secs(10);

// The pieces of core's bring-up the VFS needs, in fl_init's order - minus the two it also
// takes that are hidden as well, the arena growth watchdog and the perf-scope tree printer,
// both diagnostics nothing below reads.
extern "C" {
    fn arena_tracker_init();
    fn arena_scratch_init();
    fn error_report_init();
    fn file_watcher_init();
    fn fl_jobs_create(arena: *mut core::ffi::c_void, num_threads: i32);
}

fn spin_until(what: &str, mut ready: impl FnMut() -> bool) {
    let deadline = Instant::now() + PATIENCE;
    while !ready() {
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        std::thread::yield_now();
    }
}

/// The frame tick, which is also where a handle closed mid-flight is reclaimed.
fn pump() {
    // SAFETY: the VFS is up (see setup) and this is a non-worker thread, which is the
    // main-thread role vfs_update requires.
    unsafe { flowi_vfs_sys::vfs_update() };
}

/// The single job worker, held by a job that does not return until the test says so.
///
/// Every operation launched while this is held is queued behind it and so cannot have
/// finished, which is what makes "the close returned early" a testable claim rather than a
/// stopwatch reading.
struct Gate {
    entered: AtomicBool,
    timed_out: AtomicBool,
    released: Mutex<bool>,
    wake: Condvar,
}

impl Gate {
    /// Occupy the worker, returning once the job is actually running on it.
    fn occupy() -> Arc<Gate> {
        let gate = Arc::new(Gate {
            entered: AtomicBool::new(false),
            timed_out: AtomicBool::new(false),
            released: Mutex::new(false),
            wake: Condvar::new(),
        });
        let held = Arc::into_raw(Arc::clone(&gate)) as *mut core::ffi::c_void;
        // SAFETY: the job system is up, and hold_job takes back exactly the reference
        // handed over here - the job system runs a scheduled job exactly once.
        unsafe { flowi_core_sys::fl_jobs_add_job(Some(hold_job), held) };
        spin_until("the gate job to reach the worker", || {
            gate.entered.load(Ordering::Acquire)
        });
        gate
    }

    fn hold(&self) {
        self.entered.store(true, Ordering::Release);
        let mut released = self
            .released
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        while !*released {
            let (guard, wait) = self
                .wake
                .wait_timeout(released, PATIENCE)
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            released = guard;
            if wait.timed_out() {
                // Set before the job returns, so a close that blocked until this fired sees
                // it and reports itself.
                self.timed_out.store(true, Ordering::Release);
                return;
            }
        }
    }

    fn release(&self) {
        *self
            .released
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = true;
        self.wake.notify_all();
    }

    /// Whether the gate is still closed - false once it gave up waiting, which is the shape
    /// a blocking close takes here.
    fn still_closed(&self) -> bool {
        !self.timed_out.load(Ordering::Acquire)
    }
}

unsafe extern "C" fn hold_job(
    user_data: *mut core::ffi::c_void,
    _info: flowi_core_sys::JobsWorkerInfo,
) {
    // SAFETY: user_data is the Arc reference Gate::occupy handed to this one job.
    let gate = unsafe { Arc::from_raw(user_data.cast::<Gate>()) };
    gate.hold();
}

/// What a transform produces: a counter it bumps when it is dropped, so a test can watch
/// the value being reclaimed with the slot that holds it.
struct DropCount(Arc<AtomicUsize>);

impl Drop for DropCount {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::Release);
    }
}

fn setup(tag: &str) -> (MutexGuard<'static, bool>, PathBuf, OwnedMount) {
    let mut process = PROCESS
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    if !*process {
        let arena = Arena::new(ARENA_RESERVE);
        // SAFETY: the one-time bring-up, in fl_init's order: the allocation tracker before
        // any arena is used, thread scratch before anything allocates from it, then the job
        // system on an arena that outlives it. One worker, so one gate job holds the pool.
        unsafe {
            arena_tracker_init();
            arena_scratch_init();
            error_report_init();
            fl_jobs_create(arena.as_raw().cast(), 1);
            file_watcher_init();
        }
        assert!(Vfs::init(&arena), "the VFS comes up");
        // Core, the job system and the VFS all live on this arena for the rest of the
        // process, and Arena is !Send so a static cannot hold it. Leaking it is the
        // lifetime: nothing here tears the process-wide state down again.
        core::mem::forget(arena);
        *process = true;
    }

    let dir = std::env::temp_dir().join(format!("flowi-vfs-{tag}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).expect("create the mount directory");
    std::fs::write(dir.join(FILE_NAME), FILE_BYTES).expect("write the file to read");

    let mount = Vfs::mount(dir.to_str().expect("a utf-8 temp path")).expect("mount");
    // Before the gate: the mount's own job needs the worker too.
    spin_until("the mount to come up", || mount.is_ready());

    (process, dir, mount)
}

/// Read the file once more, pumping the frame tick until it completes: the sweep runs, and a
/// read that still succeeds is the evidence that closing a handle mid-flight left the VFS
/// intact.
fn settle(mount: &Mount) {
    let mut witness = mount.read(FILE_NAME).expect("launch the witness read");
    let mut bytes = None;
    spin_until("the witness read to complete", || {
        pump();
        match witness.poll() {
            Poll::Pending => false,
            Poll::Ready(result) => {
                bytes = Some(result.expect("the witness read succeeds"));
                true
            }
        }
    });
    assert_eq!(bytes.expect("witness bytes").as_slice(), FILE_BYTES);
}

fn teardown(dir: PathBuf, mount: OwnedMount) {
    // Closing the mount drains its jobs and frees whatever the sweep has not, so a handle
    // freed twice would come apart here.
    drop(mount);
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn dropping_a_pending_read_returns_while_the_worker_is_gated() {
    let (_process, dir, mount) = setup("read-drop");
    let gate = Gate::occupy();

    let mut handle = mount.read(FILE_NAME).expect("launch the gated read");
    assert!(
        matches!(handle.poll(), Poll::Pending),
        "the gated worker cannot have finished the read"
    );

    drop(handle);
    assert!(
        gate.still_closed(),
        "closing the handle waited out the gated read instead of returning"
    );

    gate.release();
    settle(&mount);
    teardown(dir, mount);
}

#[test]
fn a_dropped_mapped_read_keeps_its_slot_for_the_worker() {
    let (_process, dir, mount) = setup("read-with-drop");
    let gate = Gate::occupy();

    let ran = Arc::new(AtomicBool::new(false));
    let drops = Arc::new(AtomicUsize::new(0));
    let (ran_in, drops_in) = (Arc::clone(&ran), Arc::clone(&drops));

    // The closure and the value it produces are the pair the slot has to keep alive - the
    // closure until the worker calls it, the value until the slot itself goes. What the
    // transform is handed is not asserted on: a close cancels, so the read may well be
    // stopped before it has any bytes, and either way the callback is invoked once.
    let mut read = mount
        .read_with(FILE_NAME, move |_bytes| {
            ran_in.store(true, Ordering::Release);
            DropCount(drops_in)
        })
        .expect("launch the gated mapped read");
    assert!(
        matches!(read.poll(), Poll::Pending),
        "the gated worker cannot have finished the read"
    );

    drop(read);
    assert!(
        gate.still_closed(),
        "closing the mapped read waited out the gated read instead of returning"
    );
    assert!(
        !ran.load(Ordering::Acquire),
        "the transform cannot have run behind the gate"
    );

    // The transform runs after its owner is gone, which is the case the slot has to survive:
    // it still holds the closure the worker is about to call.
    gate.release();
    spin_until("the worker to run the transform", || {
        ran.load(Ordering::Acquire)
    });
    spin_until("the slot to be freed", || {
        drops.load(Ordering::Acquire) == 1
    });

    settle(&mount);
    assert_eq!(
        drops.load(Ordering::Acquire),
        1,
        "the value the transform produced is freed exactly once"
    );
    teardown(dir, mount);
}

#[test]
fn cancelling_a_task_returns_while_the_worker_is_gated() {
    let (_process, dir, mount) = setup("task-cancel");
    let gate = Gate::occupy();

    let executor = LocalExecutor::new();
    let handle = mount.read(FILE_NAME).expect("launch the gated read");
    let task = executor.spawn(async move { handle.await });
    executor.tick();
    assert!(
        !task.is_finished(),
        "the gated worker cannot have finished the read"
    );

    drop(task);
    assert!(
        gate.still_closed(),
        "cancelling the task waited out the gated read instead of returning"
    );

    gate.release();
    settle(&mount);
    teardown(dir, mount);
}
