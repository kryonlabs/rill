/* Native desktop surfaces use the same framebuffer and input stream as Rill.
 * rio copies only these panel/menu rectangles above application windows. */
#ifdef KRYON_NATIVE_PLAN9
static char overlay_command[1024];
static int overlay_length;
static int overlay_count;
static int overlay_fd = -2;

static void
plan9_overlay_begin(void)
{
    strcpy(overlay_command, "overlay");
    overlay_length = 7;
    overlay_count = 0;
}

static void
plan9_overlay_rect(Rectangle rect)
{
    if(overlay_count >= 16 || rect.width <= 0 || rect.height <= 0)
        return;
    overlay_length += snprintf(overlay_command + overlay_length,
                                sizeof(overlay_command) - overlay_length,
                                " %d %d %d %d", (int)rect.x, (int)rect.y,
                                (int)(rect.x + rect.width),
                                (int)(rect.y + rect.height));
    overlay_count++;
}

static void
plan9_overlay_end(void)
{
    char state[128];
    int fd, n;

    if(overlay_fd == -2) {
        overlay_fd = -1;
        fd = open("/dev/winfo", OREAD);
        if(fd >= 0) {
            n = read(fd, state, sizeof(state) - 1);
            close(fd);
            if(n > 0) {
                state[n] = '\0';
                if(strstr(state, " desktop") != NULL)
                    overlay_fd = open("/dev/wctl", OWRITE | OCEXEC);
            }
        }
    }
    if(overlay_fd >= 0 &&
       write(overlay_fd, overlay_command, overlay_length) != overlay_length) {
        fprintf(stderr, "rill: could not publish desktop overlays: %r\n");
        close(overlay_fd);
        overlay_fd = -1;
    }
}
#else
static void plan9_overlay_begin(void) {}
static void plan9_overlay_rect(Rectangle rect) { (void)rect; }
static void plan9_overlay_end(void) {}
#endif
