#include <iostream>
#include <cstring>
using namespace std;

class MyString {
private:
    char* data;
public:
    // Konstruktor: Allokiert Speicher und kopiert den String.
    MyString(const char* str) {
        data = new char[strlen(str) + 1];
        strcpy(data, str);
    }
    // Destruktor: Gibt den allokierten Speicher frei.
    ~MyString() {
        delete [] data;
    }
    // Es fehlt ein eigener Kopierkonstruktor!
};

void printMyString(MyString s) {
    // Übergabe per Wert führt zum Aufruf des Standard-Kopierkonstruktors.
    cout << "String: " << s.data << endl;
}

int main() {
    MyString s("Hello, world!");
    printMyString(s);
    // s wird hier zerstört, ebenso wie die Kopie in printMyString.
    // Beide Objekte versuchen, den gleichen Speicherbereich freizugeben.
    return 0;
}