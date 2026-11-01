#pragma once
#include <hyprutils/memory/WeakPtr.hpp>
namespace Offload {
// Queue entries have unique ownership. Borrow only for the synchronous signal
// callback; converting this weak reference into shared ownership would abort.
template<class T>
T* borrowQueuedState(const Hyprutils::Memory::CWeakPointer<T>& state) noexcept {
    return state.expired()?nullptr:state.get();
}
}
