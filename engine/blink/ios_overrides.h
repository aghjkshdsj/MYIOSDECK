/* Force-included into every Blink source compiled for MYIOSDECK.
 * Blink is a command-line program: on fatal errors it calls exit(), which in
 * an iOS app would close the whole app. Route it to the driver, which unwinds
 * back to the caller instead (engine/blink/myiosdeck_driver.c). */
#define exit mid_blink_exit
