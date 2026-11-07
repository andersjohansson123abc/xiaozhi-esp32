#ifndef PASSPORT_TLS_GATE_H
#define PASSPORT_TLS_GATE_H

#include <atomic>

// One TLS session beside the voice channel is what the heap on this board can
// take, so the monitors take turns. The heap limits they check before a fetch say
// nothing about a session that the other monitor is just about to open.
inline std::atomic<bool> passport_tls_busy{false};

inline bool TryTakeTlsTurn() {
    bool expected = false;
    return passport_tls_busy.compare_exchange_strong(expected, true);
}

inline void ReleaseTlsTurn() {
    passport_tls_busy = false;
}

#endif  // PASSPORT_TLS_GATE_H
