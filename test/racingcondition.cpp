#include <iostream>
#include <thread>
#include <vector>
using namespace std;

int counter = 0;

void incrementCounter() {
    for (int i = 0; i < 1000000; ++i) {
        // Ungeschützter Zugriff auf counter – Race-Condition
        counter++;
    }
}

int main() {
    vector<thread> threads;
    // Starte 10 Threads, die jeweils counter um 1.000.000 inkrementieren sollen
    for (int i = 0; i < 10; ++i) {
        threads.push_back(thread(incrementCounter));
    }
    for (auto& t : threads) {
        t.join();
    }
    cout << "Endwert von counter: " << counter << endl;
    return 0;
}