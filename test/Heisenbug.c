#include <stdio.h>
#include <string.h>

int main(void) {
    char buf[8];

    // Absichtlich KEIN Null-Byte am Ende:
    memcpy(buf, "ABCDEFGH", 8);

    // Beobachtung verändert das Timing/Stacklayout:
    // printf("debug: buf prepared\n");

    // UB: printf erwartet eine '\0'-terminierte C-String.
    // Läuft weiter im Speicher, bis zufällig ein 0-Byte kommt.
    printf("buf = '%s'\n", buf);

    return 0;
}
