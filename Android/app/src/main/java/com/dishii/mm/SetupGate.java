package com.dishii.mm;

// The game's thread waits here (MainActivity.waitForSetupFromNative, from mm/src/code/main.c) until the port's files
// are in place. Unlike the CountDownLatch it replaces, the wait ends one way or the other: open() when the files
// are ready, cancel() when the person leaves, the app closes or the time runs out. The game only starts on open().
// Plain Java, tested in src/test.
final class SetupGate {

    enum State { WAITING, READY, CANCELLED }

    interface Waiting {
        // Called every `reportEveryMs` while still waiting, with how long it has been.
        void stillWaiting(long waitedMs);
    }

    private State state = State.WAITING;
    private String reason = "";

    synchronized void open() {
        if (state == State.WAITING) {
            state = State.READY;
            notifyAll();
        }
    }

    synchronized void cancel(String why) {
        if (state == State.WAITING) {
            state = State.CANCELLED;
            reason = why;
            notifyAll();
        }
    }

    synchronized State state() {
        return state;
    }

    synchronized String reason() {
        return reason;
    }

    // True once open(); false after cancel() or `timeoutMs` (the gate is then cancelled, so it cannot open later).
    synchronized boolean await(long timeoutMs, long reportEveryMs, Waiting waiting) throws InterruptedException {
        long start = System.currentTimeMillis();
        long nextReport = reportEveryMs;
        while (state == State.WAITING) {
            long waited = System.currentTimeMillis() - start;
            if (waited >= timeoutMs) {
                state = State.CANCELLED;
                reason = "tempo esgotado (" + (timeoutMs / 1000) + " s)";
                notifyAll();
                break;
            }
            if (waiting != null && waited >= nextReport) {
                waiting.stillWaiting(waited);
                nextReport += reportEveryMs;
            }
            wait(Math.max(1, Math.min(1000, Math.min(timeoutMs - waited, nextReport - waited))));
        }
        return state == State.READY;
    }
}
