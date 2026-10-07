#ifndef ACE_CLI_H
#define ACE_CLI_H

/* Entry points of the two tools, shared by the unified `ace` binary.
 * mkace: archive creation, unace: extraction/listing (historical names,
 * also reachable through compatibility symlinks). */
int mkace_main(int argc, char **argv);
int unace_main(int argc, char **argv);

#endif /* ACE_CLI_H */
