#ifndef MOCK_VIRTERROR_H
#define MOCK_VIRTERROR_H

typedef struct {
    int code;
    char *message;
} virError, *virErrorPtr;

virErrorPtr virGetLastError(void);
void virResetLastError(void);

#endif
