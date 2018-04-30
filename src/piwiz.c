#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#include <libintl.h>

#define PAGE_INTRO 0
#define PAGE_LOCALE 1
#define PAGE_PASSWD 2
#define PAGE_WIFIAP 3
#define PAGE_WIFIPSK 4
#define PAGE_DONE 5

/* Controls */

static GtkWidget *main_dlg, *msg_dlg;
static GtkWidget *wizard_nb, *next_btn, *prev_btn, *skip_btn;
static GtkWidget *country_cb, *language_cb, *timezone_cb;
static GtkWidget *ap_tv, *psk_label;
static GtkWidget *pwd1_te, *pwd2_te, *psk_te;

/* Lists for localisation */

GtkListStore *locale_list, *tz_list;
GtkTreeModelSort *scount, *slang, *scity;
GtkTreeModelFilter *fcount, *flang, *fcity;

/* List of APs */

GtkListStore *ap_list;

char wifi_if[16];
char *ssid;
gint conn_timeout = 0;

/* Functions in dhcpcd-gtk/main.c */

void select_ssid (char *ssid, const char *psk);
void init_dhcpcd (void);
extern void *con; // not really a void *, but let's keep the dhcpcd stuff out where possible...


/* Helpers */

static int get_status (char *cmd)
{
    FILE *fp = popen (cmd, "r");
    char buf[64];
    int res;

    if (fp == NULL) return 0;
    if (fgets (buf, sizeof (buf) - 1, fp) != NULL)
    {
        sscanf (buf, "%d", &res);
        pclose (fp);
        return res;
    }
    pclose (fp);
    return 0;
}

static void get_string (char *cmd, char *name)
{
    FILE *fp = popen (cmd, "r");
    char buf[64];

    name[0] = 0;
    if (fp == NULL) return;
    if (fgets (buf, sizeof (buf) - 1, fp) != NULL)
    {
        sscanf (buf, "%s", name);
    }
    pclose (fp);
}


static int get_quoted_param (char *path, char *fname, char *toseek, char *result)
{
    char buffer[256], *linebuf = NULL, *cptr, *dptr;
    int len = 0;

    sprintf (buffer, "%s/%s", path, fname);
    FILE *fp = fopen (buffer, "rb");
    if (!fp) return 0;

    while (getline (&linebuf, &len, fp) > 0)
    {
        // skip whitespace at line start
        cptr = linebuf;
        while (*cptr == ' ' || *cptr == '\t') cptr++;

        // compare against string to find
        if (!strncmp (cptr, toseek, strlen (toseek)))
        {
            // find string in quotes
            strtok (cptr, "\"");
            dptr = strtok (NULL, "\"\n\r");

            // copy to dest
            if (dptr) strcpy (result, dptr);
            else result[0] = 0;

            // done
            free (linebuf);
            fclose (fp);
            return 1;
        }
    }

    // end of file with no match
    result[0] = 0;
    free (linebuf);
    fclose (fp);
    return 0;
}

static int vsystem (const char *fmt, ...)
{
    char buffer[1024];
    va_list arg;
    va_start (arg, fmt);
    vsprintf (buffer, fmt, arg);
    va_end (arg);
    return system (buffer);
}

static gboolean ok_clicked (gpointer data)
{
    gtk_widget_destroy (GTK_WIDGET (msg_dlg));
    return FALSE;
}


static void message (char *msg, int wait)
{
    GdkColor col;
    GtkWidget *wid;
    GtkBuilder *builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/piwiz.ui", NULL);

    msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "msg");
    gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));

    wid = (GtkWidget *) gtk_builder_get_object (builder, "msg_eb");
    gdk_color_parse ("#FFFFFF", &col);
    gtk_widget_modify_bg (wid, GTK_STATE_NORMAL, &col);

    wid = (GtkWidget *) gtk_builder_get_object (builder, "msg_lbl");
    gtk_label_set_text (GTK_LABEL (wid), msg);

    wid = (GtkWidget *) gtk_builder_get_object (builder, "msg_bb");

    gtk_widget_show_all (msg_dlg);
    if (!wait) gtk_widget_set_visible (wid, FALSE);
    else
    {
        wid = (GtkWidget *) gtk_builder_get_object (builder, "msg_btn");
        g_signal_connect (wid, "clicked", G_CALLBACK (ok_clicked), NULL);
    }
    g_object_unref (builder);
}

static gboolean close_msg (gpointer data)
{
    gtk_widget_destroy (GTK_WIDGET (msg_dlg));
    gtk_notebook_next_page (GTK_NOTEBOOK (wizard_nb));
    return FALSE;
}

void connect_success (void)
{
    if (conn_timeout)
    {
        gtk_timeout_remove (conn_timeout);
        conn_timeout = 0;
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        gtk_notebook_set_current_page (GTK_NOTEBOOK (wizard_nb), PAGE_DONE);
    }
}

gint connect_failure (gpointer data)
{
    conn_timeout = 0;
    gtk_widget_destroy (GTK_WIDGET (msg_dlg));
    message (_("Failed to connect to network."), 1);
    return FALSE;
}

static gpointer set_locale (gpointer data)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    char *cc, *lc, *city, *ext, *lcc;
    FILE *fp;
    
    // get the combo entries and look up relevant codes in database
    model = gtk_combo_box_get_model (GTK_COMBO_BOX (language_cb));
    gtk_combo_box_get_active_iter (GTK_COMBO_BOX (language_cb), &iter);
    gtk_tree_model_get (model, &iter, 0, &lc, -1);
    gtk_tree_model_get (model, &iter, 1, &cc, -1);
    gtk_tree_model_get (model, &iter, 4, &ext, -1);
    lcc = g_ascii_strdown (cc, -1);

    model = gtk_combo_box_get_model (GTK_COMBO_BOX (timezone_cb));
    gtk_combo_box_get_active_iter (GTK_COMBO_BOX (timezone_cb), &iter);
    gtk_tree_model_get (model, &iter, 0, &city, -1);

    // set timezone
    fp = fopen ("/etc/timezone", "wb");
    fprintf (fp, "%s\n", city);
    fclose (fp);

    // set wifi country
    vsystem ("wpa_cli -i %s set country %s", wifi_if, cc);
    vsystem ("iw reg set %s", cc);
    vsystem ("wpa_cli -i %s save_config", wifi_if);

    // set keyboard
    fp = fopen ("/etc/default/keyboard", "wb");
    fprintf (fp, "XKBMODEL=pc105\nXKBLAYOUT=%s\nXKBVARIANT=\nXKBOPTIONS=\nBACKSPACE=guess", lcc);
    fclose (fp);
    vsystem ("setxkbmap -layout %s -variant \"\" -option \"\"", lcc);

    // set locale
    vsystem ("sed -i /etc/locale.gen -e 's/^\\([^#].*\\)/# \\1/g'");
    vsystem ("sed -i /etc/locale.gen -e 's/^# \\(%s_%s[\\. ].*UTF-8\\)/\\1/g'", lc, cc);
    vsystem ("locale-gen");
    vsystem ("LC_ALL=%s_%s%s LANG=%s_%s%s LANGUAGE=%s_%s%s update-locale LANG=%s_%s%s LC_ALL=%s_%s%s LANGUAGE=%s_%s%s", lc, cc, ext, lc, cc, ext, lc, cc, ext, lc, cc, ext, lc, cc, ext, lc, cc, ext);
    
    g_idle_add (close_msg, NULL);
    return NULL;
}

static gboolean unique_rows (GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    GtkTreeIter next = *iter;
    char *str1, *str2;
    if (!gtk_tree_model_iter_next (model, &next)) return TRUE;
    gtk_tree_model_get (model, iter, 1, &str1, -1);
    gtk_tree_model_get (model, &next, 1, &str2, -1);
    if (!strcmp (str1, str2)) return FALSE;
    return TRUE;
}

static void read_locales (void)
{
    char ccode[16], lcode[16], cname[100], lname[100], ext[8];
    char buffer[1024], *cptr;
    GtkTreeIter iter;
    FILE *fp;

    // populate the locale database
    fp = fopen ("/usr/share/i18n/SUPPORTED", "rb");
    while (fgets (buffer, sizeof (buffer) - 1, fp))
    {
        // does the line contain UTF-8; ignore lines with an @
        if (strstr (buffer, "UTF-8") && !strstr (buffer, "@"))
        {
            if (strstr (buffer, ".UTF-8")) strcpy (ext, ".UTF-8");
            else ext[0] = 0;

            if (sscanf (buffer, "%[^_]_%[^. ]", lcode, ccode) == 2)
            {
                sprintf (buffer, "%s_%s", lcode, ccode);
                get_quoted_param ("/usr/share/i18n/locales", buffer, "territory", cname);
                get_quoted_param ("/usr/share/i18n/locales", buffer, "language", lname);
                
                // deal with the likes of "malta"...
                if (cname[0] >= 'a' && cname[0] <= 'z') cname[0] -= 32;
                if (lname[0] >= 'a' && lname[0] <= 'z') lname[0] -= 32;

                gtk_list_store_append (locale_list, &iter);
                gtk_list_store_set (locale_list, &iter, 0, lcode, 1, ccode, 2, lname, 3, cname, 4, ext, -1);
            }
        }
    }
    fclose (fp);

    // sort and filter the database to produce the list for the country combo
    scount = GTK_TREE_MODEL_SORT (gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (locale_list)));
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (scount), 3, GTK_SORT_ASCENDING);
    fcount = GTK_TREE_MODEL_FILTER (gtk_tree_model_filter_new (GTK_TREE_MODEL (scount), NULL));
    gtk_tree_model_filter_set_visible_func (fcount, (GtkTreeModelFilterVisibleFunc) unique_rows, NULL, NULL);

    // populate the timezone database
    fp = fopen ("/usr/share/zoneinfo/zone.tab", "rb");
    while (fgets (buffer, sizeof (buffer) - 1, fp))
    {
        // ignore lines starting #
        if (buffer[0] != '#')
        {
            if (sscanf (buffer, "%s\t%*s\t%s", ccode, cname) == 2)
            {
                // take the final part of the string; convert _ to space
                cptr = strrchr (cname, '/');
                if (cptr) strcpy (lname, cptr + 1);
                else strcpy (lname, cname);
                cptr = lname;
                while (*cptr++) if (*cptr == '_') *cptr = ' ';

                gtk_list_store_append (tz_list, &iter);
                gtk_list_store_set (tz_list, &iter, 0, cname, 1, ccode, 2, lname, -1);
            }
        }
    }
    fclose (fp);
}

static gboolean match_country (GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    char *str;
    gtk_tree_model_get (model, iter, 1, &str, -1);
    if (!strcmp (str, (char *) data)) return TRUE;
    return FALSE;
}

static void country_changed (GtkComboBox *cb, gpointer ptr)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    char *str;

    // get the current country code from the combo box
    model = gtk_combo_box_get_model (GTK_COMBO_BOX (country_cb));
    gtk_combo_box_get_active_iter (GTK_COMBO_BOX (country_cb), &iter);
    gtk_tree_model_get (model, &iter, 1, &str, -1);

    // filter and sort the master database for entries matching this code
    flang = GTK_TREE_MODEL_FILTER (gtk_tree_model_filter_new (GTK_TREE_MODEL (locale_list), NULL));
    gtk_tree_model_filter_set_visible_func (flang, (GtkTreeModelFilterVisibleFunc) match_country, str, NULL);
    slang = GTK_TREE_MODEL_SORT (gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (flang)));
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (slang), 2, GTK_SORT_ASCENDING);

    // set up the combo box from the sorted and filtered list
    gtk_combo_box_set_model (GTK_COMBO_BOX (language_cb), GTK_TREE_MODEL (slang));
    gtk_combo_box_set_active (GTK_COMBO_BOX (language_cb), 0);

    // set the timezones for the country
    fcity = GTK_TREE_MODEL_FILTER (gtk_tree_model_filter_new (GTK_TREE_MODEL (tz_list), NULL));
    gtk_tree_model_filter_set_visible_func (fcity, (GtkTreeModelFilterVisibleFunc) match_country, str, NULL);
    scity = GTK_TREE_MODEL_SORT (gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (fcity)));
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (scity), 2, GTK_SORT_ASCENDING);

    // set up the combo box from the sorted and filtered list
    gtk_combo_box_set_model (GTK_COMBO_BOX (timezone_cb), GTK_TREE_MODEL (scity));
    gtk_combo_box_set_active (GTK_COMBO_BOX (timezone_cb), 0);
}


void scans_clear (void)
{
    gtk_list_store_clear (ap_list);
}

void scans_add (char *str, int match, int secure, int signal)
{
    GtkTreeIter iter;
    GdkPixbuf *sec_icon = NULL, *sig_icon = NULL;
    char icon_buf[64];
    int dsig;
    
    gtk_list_store_append (ap_list, &iter);
    if (secure)
        sec_icon = gtk_icon_theme_load_icon (gtk_icon_theme_get_default(), "network-wireless-encrypted", 16, 0, NULL);
    if (signal >= 0)
    {
        if (signal > 80) dsig = 100;
        else if (signal > 55) dsig = 75;
        else if (signal > 30) dsig = 50;
        else if (signal > 5) dsig = 25;
        else dsig = 0;

        sprintf (icon_buf, "network-wireless-connected-%02d", dsig);
        sig_icon = gtk_icon_theme_load_icon (gtk_icon_theme_get_default(), icon_buf, 16, 0, NULL);
    }
    gtk_list_store_set (ap_list, &iter, 0, str, 1, sec_icon, 2, sig_icon, 3, secure, -1);

    if (match)
        gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (ap_tv)), &iter);
}

int find_line (char **ssid, int *sec)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTreeSelection *sel;

    sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (ap_tv));
    if (sel && gtk_tree_selection_get_selected (sel, &model, &iter))
    {
        gtk_tree_model_get (model, &iter, 0, ssid, 3, sec, -1);
        return 1;
    } 
    return 0;
}


static void page_changed (GtkNotebook *notebook, GtkNotebookPage *page, int pagenum, gpointer data)
{
    switch (pagenum)
    {
        case PAGE_INTRO :   gtk_widget_set_sensitive (prev_btn, FALSE);
                            break;

        case PAGE_DONE :    gtk_button_set_label (GTK_BUTTON (next_btn), _("Quit"));
                            gtk_widget_set_sensitive (skip_btn, FALSE);
                            break;

        case PAGE_WIFIAP :  if (!con)
                            {
                                init_dhcpcd ();
                                scans_clear ();
                                scans_add (_("Searching for networks - please wait..."), 0, 0, -1);
                            }
                            // fallthrough...

        default :           gtk_button_set_label (GTK_BUTTON (next_btn), _("OK"));
                            gtk_widget_set_sensitive (prev_btn, TRUE);
                            gtk_widget_set_sensitive (next_btn, TRUE);
                            gtk_widget_set_sensitive (skip_btn, TRUE);
                            break;
    }
}


static void next_page (GtkButton* btn, gpointer ptr)
{
    const char *psk;
    int sec;
    char buffer[512];
    const char *pw1, *pw2;

    switch (gtk_notebook_get_current_page (GTK_NOTEBOOK (wizard_nb)))
    {
        case PAGE_LOCALE :  message (_("Setting locale - please wait..."), 0);
                            g_thread_new (NULL, set_locale, NULL);
                            break;

        case PAGE_PASSWD :  pw1 = gtk_entry_get_text (GTK_ENTRY (pwd1_te));
                            pw2 = gtk_entry_get_text (GTK_ENTRY (pwd2_te));
                            if (strlen (pw1) || strlen (pw2))
                            {
                                if (strlen (pw1) != strlen (pw2) || strcmp (pw1, pw2))
                                {
                                    message (_("The two passwords entered do not match."), 1);
                                    break;
                                }
                                vsystem ("(echo \"%s\" ; echo \"%s\") | passwd $SUDO_USER", pw1, pw2);
                            }
                            if (!wifi_if[0]) gtk_notebook_set_current_page (GTK_NOTEBOOK (wizard_nb), PAGE_DONE);
                            else gtk_notebook_next_page (GTK_NOTEBOOK (wizard_nb));
                            break;

        case PAGE_WIFIAP :  if (ssid) g_free (ssid);
                            ssid = NULL;
                            if (!find_line (&ssid, &sec))
                                gtk_notebook_set_current_page (GTK_NOTEBOOK (wizard_nb), PAGE_DONE);
                            else
                            {
                                if (sec)
                                {
                                    sprintf (buffer, _("Enter the password for the WiFi network \"%s\""), ssid);
                                    gtk_label_set_text (GTK_LABEL (psk_label), buffer);
                                    gtk_notebook_next_page (GTK_NOTEBOOK (wizard_nb));
                                }
                                else
                                {
                                    select_ssid (ssid, NULL);
                                    message (_("Connecting to WiFi network - please wait..."), 0);
                                    conn_timeout = gtk_timeout_add (30000, connect_failure, NULL);
                                }
                            }
                            break;

        case PAGE_WIFIPSK : psk = gtk_entry_get_text (GTK_ENTRY (psk_te));
                            select_ssid (ssid, psk);
                            message (_("Connecting to WiFi network - please wait..."), 0);
                            conn_timeout = gtk_timeout_add (30000, connect_failure, NULL);
                            break;

        default :           gtk_notebook_next_page (GTK_NOTEBOOK (wizard_nb));
                            break;
    }
}

static void prev_page (GtkButton* btn, gpointer ptr)
{
    if (gtk_notebook_get_current_page (GTK_NOTEBOOK (wizard_nb)) == PAGE_DONE)
        gtk_notebook_set_current_page (GTK_NOTEBOOK (wizard_nb), PAGE_WIFIAP);
    else
        gtk_notebook_prev_page (GTK_NOTEBOOK (wizard_nb));
}

static void skip_page (GtkButton* btn, gpointer ptr)
{
    switch (gtk_notebook_get_current_page (GTK_NOTEBOOK (wizard_nb)))
    {
        case PAGE_LOCALE :
        case PAGE_PASSWD :  gtk_notebook_next_page (GTK_NOTEBOOK (wizard_nb));
                            break;

        case PAGE_WIFIAP :  gtk_notebook_set_current_page (GTK_NOTEBOOK (wizard_nb), PAGE_DONE);
                            break;
    }
}


static void set_init_country (char *cc)
{
    GtkTreeIter iter;
    char *val;

    gtk_tree_model_get_iter_first (GTK_TREE_MODEL (fcount), &iter);
    while (1)
    {
        gtk_tree_model_get (GTK_TREE_MODEL (fcount), &iter, 1, &val, -1);
        if (!strcmp (cc, val))
        {
            gtk_combo_box_set_active_iter (GTK_COMBO_BOX (country_cb), &iter);
            return;
        }
        if (!gtk_tree_model_iter_next (GTK_TREE_MODEL (fcount), &iter)) break;
    }
}


/* The dialog... */

int main (int argc, char *argv[])
{
    GtkBuilder *builder;
    GtkWidget *wid;
    GtkCellRenderer *col;
    
    // get the wifi device name, if any
    get_string ("for dir in /sys/class/net/*/wireless; do if [ -d \"$dir\" ] ; then basename \"$(dirname \"$dir\")\" ; fi ; done | head -n 1", wifi_if);
                        
#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    // GTK setup
    gdk_threads_init ();
    gdk_threads_enter ();
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    // create the master databases
    locale_list = gtk_list_store_new (5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    tz_list = gtk_list_store_new (3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    ap_list = gtk_list_store_new (4, G_TYPE_STRING, GDK_TYPE_PIXBUF, GDK_TYPE_PIXBUF, G_TYPE_INT);

    // build the UI
    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/piwiz.ui", NULL);

    main_dlg = (GtkWidget *) gtk_builder_get_object (builder, "wizard_dlg");
    wizard_nb = (GtkWidget *) gtk_builder_get_object (builder, "wizard_nb");
    g_signal_connect (wizard_nb, "switch-page", G_CALLBACK (page_changed), NULL);

    next_btn = (GtkWidget *) gtk_builder_get_object (builder, "next_btn");
    g_signal_connect (next_btn, "clicked", G_CALLBACK (next_page), NULL);

    prev_btn = (GtkWidget *) gtk_builder_get_object (builder, "prev_btn");
    g_signal_connect (prev_btn, "clicked", G_CALLBACK (prev_page), NULL);
    gtk_widget_set_sensitive (prev_btn, FALSE);

    skip_btn = (GtkWidget *) gtk_builder_get_object (builder, "skip_btn");
    g_signal_connect (skip_btn, "clicked", G_CALLBACK (skip_page), NULL);

    pwd1_te = (GtkWidget *) gtk_builder_get_object (builder, "p2pwd1");
    pwd2_te = (GtkWidget *) gtk_builder_get_object (builder, "p2pwd2");
    psk_te = (GtkWidget *) gtk_builder_get_object (builder, "p4psk");
    psk_label = (GtkWidget *) gtk_builder_get_object (builder, "p4info");

    gtk_entry_set_visibility (GTK_ENTRY (pwd1_te), FALSE);
    gtk_entry_set_visibility (GTK_ENTRY (pwd2_te), FALSE);

    // set up the locale combo boxes
    read_locales ();
    wid = (GtkWidget *) gtk_builder_get_object (builder, "p1table");
    country_cb = gtk_combo_box_new_with_model (GTK_TREE_MODEL (fcount));
    language_cb = gtk_combo_box_new ();
    timezone_cb = gtk_combo_box_new ();
    gtk_table_attach (GTK_TABLE (wid), GTK_WIDGET (country_cb), 1, 2, 0, 1, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 0);
    gtk_table_attach (GTK_TABLE (wid), GTK_WIDGET (language_cb), 1, 2, 1, 2, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 0);
    gtk_table_attach (GTK_TABLE (wid), GTK_WIDGET (timezone_cb), 1, 2, 2, 3, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 0);

    // set up cell renderers to associate list columns with combo boxes
    col = gtk_cell_renderer_text_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (country_cb), col, FALSE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (country_cb), col, "text", 3);
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (language_cb), col, FALSE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (language_cb), col, "text", 2);
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (timezone_cb), col, FALSE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (timezone_cb), col, "text", 2);

    // initialise the country combo
    g_signal_connect (country_cb, "changed", G_CALLBACK (country_changed), NULL);
    set_init_country ("GB");

    gtk_widget_show_all (GTK_WIDGET (country_cb));
    gtk_widget_show_all (GTK_WIDGET (language_cb));
    gtk_widget_show_all (GTK_WIDGET (timezone_cb));

    // set up the wifi AP list
    ap_tv = (GtkWidget *) gtk_builder_get_object (builder, "p3networks");
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (ap_tv), 0, "AP", col, "text", 0, NULL);
    gtk_tree_view_column_set_expand (gtk_tree_view_get_column (GTK_TREE_VIEW (ap_tv), 0), TRUE);
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (ap_tv), FALSE);
    gtk_tree_view_set_model (GTK_TREE_VIEW (ap_tv), GTK_TREE_MODEL (ap_list));

    col = gtk_cell_renderer_pixbuf_new ();
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (ap_tv), 1, "Security", col, "pixbuf", 1, NULL);
    gtk_tree_view_column_set_sizing (gtk_tree_view_get_column (GTK_TREE_VIEW (ap_tv), 1), GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (ap_tv), 1), 30);
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (ap_tv), 2, "Signal", col, "pixbuf", 2, NULL);
    gtk_tree_view_column_set_sizing (gtk_tree_view_get_column (GTK_TREE_VIEW (ap_tv), 2), GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (ap_tv), 2), 30);

    gtk_dialog_run (GTK_DIALOG (main_dlg));

    g_object_unref (builder);
    gtk_widget_destroy (main_dlg);
    gdk_threads_leave ();

    return 0;
}

