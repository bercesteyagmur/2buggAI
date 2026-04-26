#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
using namespace std;

struct Data {
    int value;
};

// Globaler Speicher, der für ein Data-Objekt allokiert wird
char* globalBuffer = new char[sizeof(Data)];

// Funktion, die den Inhalt des globalen Speichers inkrementiert.
// Dabei wird der Speicher mittels reinterpret_cast als Data* interpretiert.
void processData() {
    // Kurz warten, um das Timing der Threads zu beeinflussen
    this_thread::sleep_for(chrono::microseconds(1));
    Data* dataPtr = reinterpret_cast<Data*>(globalBuffer);
    // Lese, inkrementiere und schreibe zurück – ohne Synchronisation
    int temp = dataPtr->value;
    temp++;
    dataPtr->value = temp;
}

int main() {
    // Initialisierung des globalen Speichers als Data-Objekt
    Data* initData = reinterpret_cast<Data*>(globalBuffer);
    initData->value = 0;
    
    vector<thread> threads;
    // Starte 10 Threads, die jeweils 100.000 mal processData() aufrufen.
    for (int i = 0; i < 10; i++) {
        threads.push_back(thread([](){
            for (int j = 0; j < 100000; j++) {
                processData();
            }
        }));
    }
    
    for (auto &t : threads) {
        t.join();
    }
    
    cout << "Finaler Wert: " << initData->value << endl;
    delete[] globalBuffer;
    return 0;
}