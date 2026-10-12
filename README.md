# Lumo

Master meter (peak, true peak, RMS, LUFS M/S/I, LRA, crest, PLR, correlation, balance)
+ three named channel columns that read from "Tap" instances in the same Live set.

## שימוש
1. על ערוץ Master: Lumo במצב **Master**.
2. על כל ערוץ שרוצים למדוד (Kick, Bass...): Lumo במצב **Tap**, כותבים שם בשדה.
3. בפלאגין של המאסטר, בשלוש העמודות כותבים את אותם שמות (לא תלוי באותיות גדולות/קטנות).
4. העמודה הראשונה היא ה-REF: "VS REF" בשתי האחרות מראה הפרש RMS ביחס אליה.

## בנייה
```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
או פשוט לדחוף ל-GitHub: ה-Workflow בונה ל-Windows ול-macOS.
