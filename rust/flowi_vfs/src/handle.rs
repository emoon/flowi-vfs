//! Typed async handles over the flowi VFS ticket system.
//!
//! The C side runs the handle system: an async launch returns a recycled u32
//! ticket, vfs_is_ready(ticket) is the non-blocking completion check,
//! `vfs_get_*` drains the result, vfs_close releases it. A [`Handle<T>`] is that
//! ticket with ownership and a payload type - a u32 and a done flag on the stack,
//! the payload type being zero-sized, no allocation.
//!
//! A [`Handle`] is not Copy and not Clone, and it closes its ticket exactly when
//! it drops. The close is non-blocking whether or not the operation has finished:
//! it never polls to completion first. A failed launch is [`None`] rather than an
//! in-band handle value, so a live [`Handle`] always holds a real ticket.
//!
//! A [`Handle`] is !Send + !Sync by construction (its payload marker is a raw
//! pointer): it belongs to the thread that launched its operation.

use core::future::{Future, IntoFuture};
use core::marker::PhantomData;
use core::pin::Pin;
use core::task::Context;

use flowi_vfs_sys::FlError;

use crate::vfs::ffi;

pub use core::task::Poll;

/// A result type a completed VFS ticket drains into: the one hook a payload
/// implements to turn `vfs_get_*` output into an owned Rust value.
///
/// Implemented by the VFS surface's result types (file bytes, directory listing);
/// not intended for outside implementation - a [`Handle`] can only be minted by
/// this crate's launch calls, so an outside impl would never be reached.
pub trait Payload: Sized {
    /// Drain the completed operation behind ticket into an owned value. Called
    /// exactly once, only after vfs_is_ready(ticket) reported true; must copy
    /// out anything it wants to keep (the C side frees its buffers on close).
    fn drain(ticket: u32) -> Result<Self, FlError>;
}

/// An owning token onto one asynchronous VFS operation: the C u32 ticket, typed.
///
/// Poll it with [`poll`](Handle::poll) until it returns [`Poll::Ready`]. Dropping
/// it - at any point, before or after completion - closes the ticket
/// non-blockingly (see the module Drop note). A Handle is !Send + !Sync, and
/// deliberately neither Copy nor Clone - its ownership is what makes
/// use-after-close unrepresentable.
pub struct Handle<T: Payload> {
    ticket: u32,
    /// Fused-poll guard: set once the result is drained, after which another poll
    /// panics instead of re-draining the finished ticket.
    done: bool,
    /// Carries the payload type, and is what makes the handle !Send + !Sync -
    /// a ticket belongs to the thread that launched its operation.
    _marker: PhantomData<*const T>,
}

impl<T: Payload> Handle<T> {
    /// Wrap a successfully-launched operation's ticket, which must be live and
    /// unclosed - the value the launch returned.
    #[inline]
    pub(crate) fn from_ticket(ticket: u32) -> Handle<T> {
        Handle {
            ticket,
            done: false,
            _marker: PhantomData,
        }
    }

    /// Non-blocking check for completion - the C vfs_is_ready with a typed face.
    ///
    /// Returns [`Poll::Pending`] while the operation runs and [`Poll::Ready`] with
    /// its `Result<T, FlError>` the first time it observes completion. Polling
    /// again after a [`Poll::Ready`] panics - like a fused future, the result is
    /// yielded exactly once.
    pub fn poll(&mut self) -> Poll<Result<T, FlError>> {
        assert!(!self.done, "handle polled after completion");
        // SAFETY: ticket is our live, unclosed ticket (from_ticket contract).
        if !unsafe { ffi::vfs_is_ready(self.ticket) } {
            return Poll::Pending;
        }
        self.done = true;
        Poll::Ready(T::drain(self.ticket))
    }
}

impl<T: Payload> Drop for Handle<T> {
    fn drop(&mut self) {
        // Non-blocking close of the C ticket - never a poll-to-completion spin.
        // SAFETY: ticket is our ticket, closed exactly once.
        unsafe { ffi::vfs_close(self.ticket) }
    }
}

/// The [`Handle<T>`] → [`Future`] bridge: the leaf a frame-ticked local executor
/// awaits. Built with [`Handle::into_future`] (so handle.await just works), it
/// owns its [`Handle`] and yields the operation's `Result<T, FlError>` when it
/// completes.
///
/// Self-waking. The underlying async C operation completes with no path back to
/// the executor's [`Waker`](core::task::Waker), so on every [`Poll::Pending`] this future re-arms itself -
/// wakes its own waker so its task is re-polled on the next tick. That keeps a
/// pending leaf, and any combinator (join/select) composed over it, alive under a
/// genuinely waker-driven executor that polls only woken tasks.
///
/// Owning the [`Handle`] is what makes cancellation-by-drop correct: dropping the
/// awaiting task drops this future, which drops the [`Handle`] and runs its
/// non-blocking close.
pub struct HandleFuture<T: Payload> {
    /// The in-flight handle, taken to [`None`] once it yields - a fused guard so a
    /// stray re-poll after completion panics rather than re-polling the drained
    /// ticket.
    handle: Option<Handle<T>>,
}

impl<T: Payload> Future for HandleFuture<T> {
    type Output = Result<T, FlError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        // HandleFuture is Unpin (its fields are), so `Pin<&mut Self>` derefs
        // mutably - no pin projection needed.
        let handle = self
            .handle
            .as_mut()
            .expect("HandleFuture polled after completion");
        match handle.poll() {
            Poll::Ready(result) => {
                self.handle = None;
                Poll::Ready(result)
            }
            Poll::Pending => {
                // Self-wake: no external event will invoke our waker, so re-arm for
                // the next tick.
                cx.waker().wake_by_ref();
                Poll::Pending
            }
        }
    }
}

impl<T: Payload> IntoFuture for Handle<T> {
    type Output = Result<T, FlError>;
    type IntoFuture = HandleFuture<T>;

    /// Consume the handle into its awaitable [`HandleFuture`] leaf, so handle.await
    /// drives the operation to its `Result<T, FlError>` on a local executor. Takes
    /// self by value: the future owns the handle, so awaiting a task and dropping
    /// it cancels the operation.
    #[inline]
    fn into_future(self) -> HandleFuture<T> {
        HandleFuture { handle: Some(self) }
    }
}
