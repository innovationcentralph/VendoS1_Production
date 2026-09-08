#pragma once
#include <Arduino.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Simple AT-style CLI framework
//
// Same table-driven pattern as the STM32 firmware src/cli.h, which is in turn
// the same as the S1 bring-up harness s1_cli.h — so the same muscle memory
// works on every board in this family.
//
// To add a new command:
//   1. Write a handler: void my_handler(const char* args, Stream& out);
//   2. Register it in cli.cpp inside the kCommands[] table:
//        { "AT+FOO",  "Description shown by AT+HELP?",  my_handler },
//   3. The CLI compares the command name case-insensitively. Anything after
//      the command name (including '=' or '?') is passed to the handler as
//      'args' so commands can be of the form:
//        AT+FOO            (args = "")
//        AT+FOO?           (args = "?")
//        AT+FOO=1,2,3      (args = "=1,2,3")
//
// ORDERING TRAP: dispatch() takes the FIRST prefix match, so a command whose
// name is a prefix of another must be registered AFTER the longer one. e.g.
// "AT+COIN_POLARITY" must precede any bare "AT+COIN" entry, or the short one
// swallows it.
// ---------------------------------------------------------------------------

typedef void (*CliHandler)(const char* args, Stream& out);

struct CliCommand {
    const char* name;
    const char* help;
    CliHandler  handler;
};

// Feed one character into the CLI parser. When CR/LF is received, the
// accumulated line is dispatched to the matching command handler.
void cli_feed_char(char c, Stream& out);

// Get the command table (used by AT+HELP?).
const CliCommand* cli_get_commands(size_t& count);
