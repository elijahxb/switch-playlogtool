#pragma once

#include <stdbool.h>

typedef enum {
    UI_NORMAL = 0,
    UI_DIM,
    UI_ACCENT,
    UI_GOOD,
    UI_BAD,
    UI_TITLE,
    UI_PANEL,
    UI_BORDER,
    UI_SELECTED,
} UiStyle;

void uiInit(void);
void uiExit(void);
bool uiMainLoop(void);

void uiClear(void);
void uiPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void uiUpdate(void);
void uiSetPageTitle(const char *title);

void uiFrameBegin(const char *title, const char *subtitle);
void uiFrameEnd(void);
void uiSetFooter(const char *text);
void uiSetFooter2(const char *text);
void uiFill(int x, int y, int w, int h, UiStyle style);
void uiPanel(int x, int y, int w, int h);
void uiTextAt(int x, int y, int max_w, const char *text, UiStyle style);
int uiTextW(const char *text, UiStyle style);
