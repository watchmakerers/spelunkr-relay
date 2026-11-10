// The client's version. Bump it with each release tag (v1.2 -> "1.2"): the updater
// compares it with the newest release the relay knows about.
#ifndef SPELUNKR_VERSION_H
#define SPELUNKR_VERSION_H
#ifndef SPELUNKR_VERSION              // (a build can override it: -DSPELUNKR_VERSION=\"1.1\")
#define SPELUNKR_VERSION "1.3"
#endif
#endif
