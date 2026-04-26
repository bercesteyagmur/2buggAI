#include <iostream>
using namespace std;

int main() {
    const int size = 10;
    int* buffer = new int[size];

    // Massive Randeffekte: Absichtliches Schreiben weit außerhalb der Array-Grenzen.
    // Hier wird von -10 bis size+20 (also -10 bis 29) iteriert,
    // obwohl nur 10 Elemente (Index 0 bis 9) reserviert sind.
    for (int i = -10; i < size + 20; i++) {
        buffer[i] = i;  // Undefiniertes Verhalten: Out-of-Bounds-Zugriff.
    }

    // Ebenso werden Werte außerhalb der Grenzen ausgelesen.
    for (int i = -10; i < size + 20; i++) {
        cout << "buffer[" << i << "] = " << buffer[i] << endl;
    }

    delete[] buffer;
    return 0;
}