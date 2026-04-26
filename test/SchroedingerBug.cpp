#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

std::atomic<bool> ready{false};
int* p = nullptr;

void writer() {
    // "Erzeugt" Daten
    int* local = new int(123);
    p = local;                 // NICHT atomar / nicht synchronisiert (Race)
    ready.store(true, std::memory_order_release);

    // Sehr schnell wieder freigeben -> anderer Thread könnte p noch benutzen
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    delete local;
}

void reader() {
    while (!ready.load(std::memory_order_acquire)) {
        // busy wait
    }

    // p kann hier schon freigegeben sein -> Use-After-Free
    // Je nach Timing: klappt, liefert Müll, oder crasht.
    std::cout << "Value: " << *p << "\n";
}

int main() {
    std::thread t1(writer);
    std::thread t2(reader);

    t1.join();
    t2.join();
}
