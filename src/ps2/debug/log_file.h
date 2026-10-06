#pragma once
/* ================================================================================================
 * File: log_file.h
 * Brief: The console log on the game data's drive. Every Com_Printf line and the fatal error go
 *        to "<base path>/quake2.log", next to baseq2/. A run on the console has no stdout
 *        anyone can read, so this is how a hardware session gets a log. Each line is on the
 *        drive before the next one is printed, so a hang or a crash still leaves the log up to
 *        that point.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::debug {

// Points the log at "<basePath>/quake2.log". Called once, from main() as soon as the game data
// has been found, so the log covers the whole of Qcommon_Init. The file is emptied by the first
// line written to it, and a file that can't be created leaves the log off, with a warning on
// stdout.
void LogFileOpen(const char * basePath);

// Registers ps2_logfile, which turns the log off and on (from Sys_Init). Until then the log
// follows the cvar's default.
void LogFileRegisterCvar();

// Appends text, stamping each line with the seconds since boot. Callable from any thread.
void LogFileWrite(const char * text);

// LogFileWrite for the fatal paths (Sys_Error, the EE exception report). These can run on a
// thread whose own write the failure interrupted, still holding the log's lock, so this waits
// for the lock only when another thread holds it. It also starts on a fresh line.
void LogFileWriteFatal(const char * text);

} // namespace ps2::debug
