#include <iostream>
using namespace std;

struct Container {
    int arr[5];   // Array mit 5 Elementen
    int secret;   // Direkt im Speicher hinter dem Array abgelegt
};

int main() {
    // Initialisierung: arr enthält 1 bis 5, secret ist 0
    Container c = { { 1, 2, 3, 4, 5 }, 0 };

    // Expliziter (aber unsicherer) Out-of-Bounds-Zugriff:
    // Der Zugriff auf c.arr[5] ist außerhalb des gültigen Bereichs von arr,
    // überschreibt aber (in den meisten Implementierungen) das Feld c.secret.
    c.arr[5] = 777;  // Dies setzt c.secret auf 777.

    cout << "Secret: " << c.secret << endl;  // Erwartete Ausgabe: 777
    return 0;
}