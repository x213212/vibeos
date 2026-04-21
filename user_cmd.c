#include "user_internal.h"

void exec_single_cmd(struct Window *w, char *cmd) {
    char *out = w->out_buf; out[0] = '\0';
    if (strncmp(cmd, "ls", 2) == 0) { lib_strcpy(out, "root\nbin\n"); }
    else if (strncmp(cmd, "netsurf", 7) == 0) {
        char *url = cmd + 7; while (*url == ' ') url++;
        char launch_url[512];
        char clean_url[256];
        if (*url == '\0') url = "https://192.168.123.100/test.html";
        netsurf_normalize_target_url(url, clean_url, sizeof(clean_url));
        if (clean_url[0] == '\0' || netsurf_prepare_launch_url(clean_url, 400, 350, launch_url, sizeof(launch_url)) < 0) {
            lib_strcpy(out, "ERR: Unable to open NetSurf URL.");
            return;
        }
        int tid = -1;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!wins[i].active) {
                reset_window(&wins[i], i);
                wins[i].active = 1; wins[i].kind = WINDOW_KIND_NETSURF;
                wins[i].x = 50; wins[i].y = 50; wins[i].w = 400; wins[i].h = 350;
                lib_strcpy(wins[i].title, "NetSurf");
                netsurf_init_engine(&wins[i]);
                bring_to_front(i); tid = i; break;
            }
        }
        if (tid != -1) {
            char wget_cmd[560]; lib_strcpy(wget_cmd, "wget "); lib_strcat(wget_cmd, launch_url);
            exec_single_cmd(&wins[tid], wget_cmd);
        }
    }
    else if (strncmp(cmd, "wget ", 5) == 0) {
        char *url = cmd + 5; while (*url == ' ') url++;
        wget_queue_request("192.168.123.100", "/test.html", "test.html", 443, 1);
        wget_job.owner_win_id = w->id; lib_printf("CMD: Set Wget Owner to %d\n", w->id);
        wget_job.owner_win_id = w->id;
        lib_strcpy(out, ">> Wget Started.");
    }
    else if (strncmp(cmd, "help", 4) == 0) { lib_strcpy(out, "Commands: ls, netsurf, wget, help"); }
}
