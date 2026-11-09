// Self-update through the relay: the relay keeps a copy of the newest GitHub release
// (GET /update/latest lists it, /update/file/<name> serves its files).
#ifndef SPELUNKR_UPDATE_H
#define SPELUNKR_UPDATE_H
// 1 if the relay has a newer version with a file for this platform (its version in
// `latest`), 0 if not, <0 if the relay can't say.
int update_check(const char *host, int port, char *latest, int latest_len);
// Downloads this platform's file into the data folder; fills `path`. 0 on success.
int update_download(const char *host, int port, char *path, int path_len);
#endif
