// kWords: {label_len, x0, y0, x1, y1, fill_class} measured regions.
// Public atlas coordinates from remaster_font.WORDS (not art). SFN cell
// metrics are NOT baked here: the caller parses them at runtime from the
// user's disc (ParseSfn) since other disc versions carry other tables.
static const WordRegion kWords70[] = {
    {7, 0, 0, 133, 26, 0},
    {10, 0, 29, 212, 55, 1},
    {9, 0, 56, 199, 81, 1},
    {5, 0, 84, 220, 125, 2},
    {1, 210, 176, 256, 215, 2},
};
static const WordRegion kWords80[] = {
    {3, 135, 0, 256, 51, 3},
    {3, 160, 168, 225, 192, 4},
    {6, 145, 190, 245, 215, 4},
};
