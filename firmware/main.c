/* ============================================================================
 * main.c
 *
 * ESP-IDF requires exactly one entry point: void app_main(void). This
 * file exists solely to satisfy that requirement and immediately hand
 * off to app_main_run() in app_main.c, where the actual bootstrap logic
 * lives. Kept deliberately trivial so there is exactly one obvious place
 * (app_main.c) to look for startup behavior.
 * ========================================================================= */

/* Declared in app_main.c. Not exposed via a header since this is the
 * only caller and the two files are tightly coupled by design (see the
 * header comment above). */
extern void app_main_run(void);

void app_main(void)
{
    app_main_run();
}
