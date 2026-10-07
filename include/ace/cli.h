#ifndef ACE_CLI_H
#define ACE_CLI_H

#define ACE_BANNER \
    "ACE 2.6 Archiver\n" \
    "(ACE Compression Software - 2005)\n" \
    "pure C reimplementation by seb3773\n" \
    "https://github.com/seb3773/ace-c\n\n"

/* Entry points of the two tools, shared by the unified `ace` binary.
 * mkace: archive creation, unace: extraction/listing (historical names,
 * also reachable through compatibility symlinks). */
int mkace_main(int argc, char **argv);
int unace_main(int argc, char **argv);

#endif /* ACE_CLI_H */
