#pragma once

// Allocator con malloc para contenedores del port que viven toda la partida.
// El operator new global sigue a JKRHeap::sCurrentHeap: un std::vector o
// std::deque estático que crece durante una sección guarda memoria de ese heap
// y queda colgando cuando la sección se destruye (crash al volver a usarlo).

#include <cstddef>
#include <cstdlib>
#include <new>

template <typename T> struct PcHostAllocator {
    using value_type = T;
    PcHostAllocator() noexcept = default;
    template <typename U> PcHostAllocator(const PcHostAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        if (count > static_cast<std::size_t>(-1) / sizeof(T)) throw std::bad_alloc();
        void* memory = std::malloc(count * sizeof(T));
        if (!memory) throw std::bad_alloc();
        return static_cast<T*>(memory);
    }
    void deallocate(T* memory, std::size_t) noexcept { std::free(memory); }
    template <typename U> bool operator==(const PcHostAllocator<U>&) const noexcept { return true; }
    template <typename U> bool operator!=(const PcHostAllocator<U>&) const noexcept { return false; }
};
