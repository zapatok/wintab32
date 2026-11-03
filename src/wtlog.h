#ifndef WTLOG_H
#define WTLOG_H

#define WTLOG_OFF    0
#define WTLOG_INIT   1
#define WTLOG_CALL   2
#define WTLOG_PACKET 3

void wtlog_open(void);
void wtlog_close(void);
void wtlog(int level, const char *fmt, ...);
int  wtlog_level(void);
const char *wtlog_path(void);

#endif
