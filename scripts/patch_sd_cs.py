# 1) D10 = AVR SS: CS idle musí zůstat OUTPUT HIGH (open-drain vracíme zpět).
# 2) Arduino File() dělá malloc(SdFile). Na UNO to po vytvoření CSV selže
#    (prázdný soubor + SD?). File je ve 2 statických slotech (DUMP potřebuje
#    root + soubor najednou).

Import("env")

from pathlib import Path

CS_MARKER = "BOILER_FVE_LCD_SAFE_CS"
FILE_MARKER = "BOILER_FVE_STATIC_FILE2"

HIGH_PATCHED = """void Sd2Card::chipSelectHigh(void) {
  /* BOILER_FVE_LCD_SAFE_CS */
  pinMode(chipSelectPin_, INPUT_PULLUP);"""

HIGH_ORIG = """void Sd2Card::chipSelectHigh(void) {
  digitalWrite(chipSelectPin_, HIGH);"""

LOW_PATCHED = """  digitalWrite(chipSelectPin_, LOW);
  pinMode(chipSelectPin_, OUTPUT);
}"""

LOW_ORIG = """  digitalWrite(chipSelectPin_, LOW);
}"""

CTOR_2SLOT = """static SdFile fileSlots[2];
static uint8_t fileSlotUsed = 0;

File::File(SdFile f, const char *n) {
  /* BOILER_FVE_STATIC_FILE2 */
  _file = 0;
  _name[0] = 0;
  for (uint8_t i = 0; i < 2; i++) {
    if (!(fileSlotUsed & (1 << i))) {
      fileSlotUsed |= (1 << i);
      _file = &fileSlots[i];
      memcpy(_file, &f, sizeof(SdFile));
      strncpy(_name, n, 12);
      _name[12] = 0;
      return;
    }
  }
}"""

CLOSE_2SLOT = """void File::close() {
  if (_file) {
    _file->close();
    for (uint8_t i = 0; i < 2; i++) {
      if (_file == &fileSlots[i]) {
        fileSlotUsed &= (uint8_t)~(1 << i);
      }
    }
    _file = 0;"""


def unpatch_cs(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if CS_MARKER not in text:
        return
    text = text.replace(HIGH_PATCHED, HIGH_ORIG, 1)
    text = text.replace(LOW_PATCHED, LOW_ORIG, 1)
    path.write_text(text, encoding="utf-8")
    print("SD CS patch: reverted", path)


def _replace_ctor(text: str) -> str:
    start = text.find("File::File(SdFile f, const char *n)")
    if start < 0:
        return text
    # include preceding static slots if we already patched once
    static_at = text.rfind("static SdFile fileSlots", 0, start)
    if static_at >= 0:
        start = static_at
    else:
        one_slot = text.rfind("static SdFile slot;", 0, start)
        if one_slot >= 0:
            start = one_slot
    brace = text.find("{", start)
    depth = 0
    i = brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + CTOR_2SLOT + text[i + 1 :]
        i += 1
    return text


def _replace_close(text: str) -> str:
    start = text.find("void File::close()")
    if start < 0:
        return text
    old = """void File::close() {
  if (_file) {
    _file->close();
    free(_file);
    _file = 0;"""
    if old in text:
        return text.replace(old, CLOSE_2SLOT, 1)
    old2 = """void File::close() {
  if (_file) {
    _file->close();
    /* BOILER_FVE_STATIC_FILE */
    _file = 0;"""
    if old2 in text:
        return text.replace(old2, CLOSE_2SLOT, 1)
    return text


def patch_file_obj(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if FILE_MARKER in text:
        return
    orig = text
    text = _replace_ctor(text)
    text = _replace_close(text)
    if text == orig:
        print("SD File patch: no match in", path)
        return
    path.write_text(text, encoding="utf-8")
    print("SD File patch: applied", path)


libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
if libdeps.exists():
    for p in libdeps.rglob("Sd2Card.cpp"):
        unpatch_cs(p)
    for p in libdeps.rglob("File.cpp"):
        patch_file_obj(p)
