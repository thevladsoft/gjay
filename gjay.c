/**
 * GJay, copyright (c) 2002 Chuck Groom
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 1, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 *
 * USAGE: gjay [--help] [-h] [-d] [-v] [-p] [-l len] [-c color]
 *
 *  --help, -h  :  Display help
 *  -d          :  Run as daemon (unattched)
 *  -v          :  Run in verbose mode. -vv for lots more info.
 *  -p          :  Generate a playlist
 *  -l len      :  Playlist length (in minutes)
 *  -c color    :  Start at color. Color may be named or hex value
 *  -u          :  Use M3U playlist format
 *  -x          :  Play playlist in XMMS
 *
 * Explanation: 
 *
 * GJay runs in, interactive (UI), daemon, or playlist-generatio mode.
 *
 * In UI mode, GJay creates playlists, displays analyzed
 * songs, and requests new songs for analysis.
 *
 * In daemon mode, GJay analyzes queued song requests. If the daemon runs in
 * 'unattached' mode, it will quit once the songs in the pending list are
 * finished.
 * 
 * In playlist mode, GJay prints a playlist and exits.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h> 
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/signal.h>
#include <sys/unistd.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <pthread.h>
#include <string.h>
#include <locale.h>
#include "gjay.h"
#include "analysis.h"
#include "ipc.h"
#include "playlist.h"

#define NUM_APPS 3
static gchar * apps[NUM_APPS] = {
    "mpg321",
    "ogg123",
    "mp3info"
};


/* Application mode -- UI, DAEMON... */
gjay_mode mode;


static gboolean app_exists (gchar * app);
static void kill_signal(int sig);
static int open_pipe(const char* filepath);

int daemon_pipe_fd;
int ui_pipe_fd;

int verbosity;


int main( int argc, char *argv[] ) 
{
    GList * list;
    char buffer[BUFFER_SIZE];
    GtkWidget * widget;
    struct stat stat_buf;
    FILE * f;
    gint i, k, hex;
    struct lconv * env;
    gboolean m3u_format, playlist_in_xmms;

    /* Insist that '.' be the decimal separator */
    setlocale(LC_NUMERIC, "C");
    env = localeconv();
    if (*env->decimal_point != '.') {
        setlocale(LC_ALL, "en_US");
        if (*env->decimal_point != '.') {
            fprintf(stderr, "Sorry, unable to set environment for proper decimal handling.\nPlease email a report to cgroom@users.sourceforge.net\n");
            return -1;
        }
    }

    srand(time(NULL));
    
    mode = UI;
    verbosity = 0;
    m3u_format = FALSE;
    playlist_in_xmms = FALSE;
    load_prefs();
    
    for (i = 0; i < argc; i++) {
        if ((strncmp(argv[i], "-h", 2) == 0) || 
            (strncmp(argv[i], "--help", 6) == 0)) {
            printf("USAGE: gjay [--help] [-hdvpux] [-l length] [-c color]\n" \
                   "\t--help, -h  :  Display this help message\n" \
                   "\t-d          :  Run as daemon\n" \
                   "\t-v          :  Run in verbose mode. -vv for lots more info\n" \
                   "\t-p          :  Generate a playlist\n" \
                   "\nPlaylist options:\n" \
                   "\t-u          :  Display list in m3u format\n" \
                   "\t-x          :  Use XMMS to play generated playlist\n" \
                   "\t-l length   :  Length of playlist, in minutes\n" \
                   "\t-c color    :  Start playlist at color, either a hex value or by name.\n" \
                   "\t               To see all options, just call -c\n");
            return 0;
        } else if (strncmp(argv[i], "-l", 2) == 0) {
            if (i + 1 < argc) {
                prefs.time = atoi(argv[i + 1]);
                i++;
            } else {
                fprintf(stderr, "Usage: -l length (in minutes)\n");
                return -1;
            }
        } else if (strncmp(argv[i], "-c", 2) == 0) {
            RGB rgb;
            if (i + 1 < argc) {
                strncpy(buffer, argv[i+1], BUFFER_SIZE);
                if (sscanf(buffer, "0x%x", &hex)) {
                    rgb.R = ((hex & 0xFF0000) >> 16) / 255.0;
                    rgb.G = ((hex & 0x00FF00) >> 8) / 255.0;
                    rgb.B = (hex & 0x0000FF) / 255.0;
                    prefs.start_color = TRUE;
                } else if (get_named_color(buffer, &rgb)) {
                        prefs.start_color = TRUE;
                }
                prefs.color = hsv_to_hb(rgb_to_hsv(rgb));
                i++;
            } else {
                fprintf(stderr, "Usage: -c color, where color is a hex number in the form 0xRRGGBB or a color name:\n%s\n", known_colors());
                return -1;
            }            
        } else if (argv[i][0] == '-') {
            for (k = 1; argv[i][k]; k++) {
                if (argv[i][k] == 'd') {
                    mode = DAEMON_DETACHED;
                    printf("Running as daemon. Ctrl+c to stop.\n");
                }
                if (argv[i][k] == 'v')
                    verbosity++;
                if (argv[i][k] == 'u')
                    m3u_format = TRUE;
                if (argv[i][k] == 'x')
                    playlist_in_xmms = TRUE;
                if (argv[i][k] == 'p') {
                    prefs.start_color = FALSE;
                    mode = PLAYLIST;
                }
            }
        }
    }

    
    /* Make sure there is a "~/.gjay" directory */
    snprintf(buffer, BUFFER_SIZE, "%s/%s", getenv("HOME"), GJAY_DIR);
    if (stat(buffer, &stat_buf) < 0) {
        if (mkdir (buffer, 
                   S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP |
                   S_IROTH | S_IXOTH) < 0) {
            fprintf (stderr, "Could not create %s\n", buffer);
            perror(NULL);
            return 0;
        }
    }


    if (mode != PLAYLIST) {
        /* Both daemon and UI app open an end of a pipe */
        daemon_pipe_fd = open_pipe(DAEMON_PIPE);
        ui_pipe_fd = open_pipe(UI_PIPE);
    }


    if(mode == UI) {
        /* Make sure a daemon is running. If not, fork. */
        gboolean fork_daemon = FALSE;
        pid_t pid;
        
        snprintf(buffer, BUFFER_SIZE, "%s/%s/%s", 
                 getenv("HOME"), GJAY_DIR, GJAY_PID);
        f = fopen(buffer, "r");
        if (f) {
            fscanf(f, "%d", &i);
            fclose(f);
            snprintf(buffer, BUFFER_SIZE, "/proc/%d/stat", i);
            if (access(buffer, R_OK))
                fork_daemon = TRUE; 
        } else {
            fork_daemon = TRUE;
        }
        if (fork_daemon) {
            pid = fork();
            if (pid < 0) {
                fprintf(stderr, "Unable to fork daemon.\n");
            } else if (pid == 0) {
                /* Daemon */
                mode = DAEMON_INIT;
            }
        }
    }

    if ((mode == UI) || (mode == PLAYLIST)) {
        songs = NULL;
        not_songs = NULL;
        songs_dirty = FALSE;
        song_name_hash    = g_hash_table_new(g_str_hash, g_str_equal);
        song_inode_hash   = g_hash_table_new(g_int_hash, g_int_equal);
        not_song_hash     = g_hash_table_new(g_str_hash, g_str_equal);

        read_data_file();
    }

    if (mode == UI) {
        if (!app_exists("xmms")) {
            fprintf(stderr, "GJay strongly suggests xmms\n"); 
        } 

        init_xmms();
        gtk_init (&argc, &argv);
        
        g_io_add_watch (g_io_channel_unix_new (daemon_pipe_fd),
                        G_IO_IN,
                        daemon_pipe_input,
                        NULL);

        widget = make_app_ui();
        gtk_widget_show_all(widget);
        set_add_files_progress_visible(FALSE);

        /* Periodically write song data to disk, if it has changed */
        gtk_timeout_add( SONG_DIRTY_WRITE_TIMEOUT, 
                         write_dirty_song_timeout, NULL);
                         
        send_ipc(ui_pipe_fd, ATTACH);
        explore_view_set_root(prefs.song_root_dir);
        set_selected_file(NULL, NULL, FALSE);

        gtk_main();

        save_prefs();
        write_data_file();

        if (prefs.detach || (prefs.daemon_action == PREF_DAEMON_DETACH)) {
            send_ipc(ui_pipe_fd, DETACH);
            send_ipc(ui_pipe_fd, UNLINK_DAEMON_FILE);
        } else {
            send_ipc(ui_pipe_fd, UNLINK_DAEMON_FILE);
            send_ipc(ui_pipe_fd, QUIT_IF_ATTACHED);
        }
  
        close(daemon_pipe_fd);
        close(ui_pipe_fd);
    } else if (mode == PLAYLIST) {
        /* Playlist mode */
        prefs.use_selected_songs = FALSE;
        prefs.rating_cutoff = FALSE;
        for (list = g_list_first(songs); list;  list = g_list_next(list)) {
            SONG(list)->in_tree = TRUE;
        }
        list = generate_playlist(prefs.time);
        if (playlist_in_xmms) {
            init_xmms();
            play_songs(list);
        } else {
            write_playlist(list, stdout, m3u_format);
        }
        g_list_free(list);
    } else {
        /* Daemon process */
        /* Write pid to ~/.gjay/gjay.pid */
        snprintf(buffer, BUFFER_SIZE, "%s/%s/%s", 
                 getenv("HOME"), GJAY_DIR, GJAY_PID);
        f = fopen(buffer, "w");
        if (f) {
            fprintf(f, "%d", getpid());
            fclose(f);
        } else {
            fprintf(stderr, "Unable to write to %s\n", GJAY_PID);
        }
        
        signal(SIGTERM, kill_signal);
        signal(SIGKILL, kill_signal);
        signal(SIGINT,  kill_signal);
        
        /* Check to see if we have all the apps we'll need for analysis */
        for (i = 0; i < NUM_APPS; i++) {
            if (!app_exists(apps[i])) {
                fprintf(stderr, "GJay requires %s\n", apps[i]); 
                return -1;
            } 
        } 
        analysis_daemon();

        /* Daemon cleans up pipes on quit */
        close(daemon_pipe_fd);
        close(ui_pipe_fd);
        unlink(DAEMON_PIPE);
        unlink(UI_PIPE);
    }
    return(0);
}


static gboolean app_exists (gchar * app) {
    FILE * f;
    char buffer[BUFFER_SIZE];
    gboolean result = FALSE;

    snprintf(buffer, BUFFER_SIZE, "which %s", app); // Yes, I'm lame
    f = popen (buffer, "r");
    while (!feof(f) && fread(buffer, 1, BUFFER_SIZE, f)) 
        result = TRUE;
    pclose(f);
    return result;
}


/**
 * When the daemon receives a kill signal, delete ~/.gjay/gjay.pid
 */
static void kill_signal (int sig) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "%s/%s/%s", 
             getenv("HOME"), GJAY_DIR, GJAY_PID);
    unlink(buffer); 
    exit(0);
}


static int open_pipe(const char* filepath) {
    int fd = -1;

    if ((fd = open(filepath, O_RDWR)) < 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "Error opening the pipe %s.\n", filepath);
            return -1;
        }

        if (mknod(filepath, S_IFIFO | 0777, 0)) {
            fprintf(stderr, "Couldn't create the pipe %s.\n", filepath);
            return -1;
        }

        if ((fd = open(filepath, O_RDWR)) < 0) {
            fprintf(stderr, "Couldn't open the pipe %s.\n", filepath);
            return -1;
        }
    }
    return fd;
}


/**
 * Read from the current file position to the end of the line ('\n'), 
 * including newline character
 */
void read_line ( FILE * f, char * buffer, int buffer_len) {
    int i;
    int result;

    for (i = 0; ((i < (buffer_len - 1)) &&
                 ((result = fgetc(f)) != EOF)); i++) {
        buffer[i] = (unsigned char) result;
        if (buffer[i] == '\n') {
            break;
        }
    }
    buffer[i] = '\0';
    return;
}



/**
 * Duplicate a string from one encoding to another
 */
gchar * strdup_convert ( const gchar * str, 
                         const gchar * enc_to, 
                         const gchar * enc_from ) {
    gchar * conv;
    gsize b_read, b_written;
    conv = g_convert (str,
                      -1, 
                      enc_to,
                      enc_from,
                      &b_read,
                      &b_written,
                      NULL);
    if (!conv) {
        printf("Unable to convert from %s charset; perhaps encoded differently?", enc_from);
        return g_strdup(str);
    }
    return conv;
}
