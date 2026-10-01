#pragma once

namespace sfe {
namespace agent {

// The lockstep link to an agent process (SokuBot's vs-COM environment), over TCP.
//
// SFE_AGENT=host:port names the agent's listening socket; the game connects as the client, so the
// agent can start listening before it launches the game and never has to find it. Text lines, one
// message per line:
//
//   game  -> agent   H <protocol> <p1 char> <p2 char> <COM level> <ticks per decision>
//                    C <the capture CSV header>
//                    S <matchState> <round> <p1 rounds won> <p2 rounds won>
//                      <p1 word x ticks> <p2 word x ticks> <capture CSV row>
//   agent -> game    A <w0> <w1> ... <w(ticks-1)>     P1's input words, one per tick
//
// The S words are what both characters actually played on the last `ticks` fight ticks, oldest
// first (zero-padded at a round's first decision): the joint action history a policy observes.
// Bit i of a word is SokuBot's BUTTONS[i] (up down left right a b c d change spell).
// S is sent every `ticks` fight ticks (matchState 2) and once on the tick a round stops (3 or 5),
// whose row carries the KO. Every S is answered by exactly one A -- the one after a round stops is
// ignored -- so the two sides can never drift out of step.

bool requested();                 // SFE_AGENT is set
bool connect(int retry_ms);       // false if it never connects within retry_ms
bool connected();
bool sendAll(const char* data, int n);
// One '\n'-terminated line, without the newline. False on timeout, close or overflow; the link is
// closed on close/overflow but NOT on a timeout, so a slow agent is retried next decision.
bool recvLine(char* buf, int cap, int timeout_ms);
void close();

} // namespace agent
} // namespace sfe
