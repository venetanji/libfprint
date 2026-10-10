/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "goodix-input.h"
static void exact_private (void) {
 g_autofree char *dir=g_dir_make_tmp("goodix-input-XXXXXX",NULL);
 g_autofree char *path=g_build_filename(dir,"factory.bin",NULL);
 guint8 source[64]={0},out[64]; GError *error=NULL;
 g_assert_true(g_file_set_contents(path,(char*)source,64,&error));
 chmod(path,0600);
 g_assert_true(goodix521d_read_private(path,out,64,&error));
 chmod(path,0644);
 g_assert_false(goodix521d_read_private(path,out,64,&error));g_clear_error(&error);
 chmod(path,0600);
 g_assert_false(goodix521d_read_private(path,out,63,&error));g_clear_error(&error);
 g_autofree char *link=g_build_filename(dir,"link",NULL);symlink(path,link);
 g_assert_false(goodix521d_read_private(link,out,64,&error));g_clear_error(&error);
 g_remove(link);g_remove(path);g_rmdir(dir);
}
static void factory_invalid (void) {
 guint8 otp[64]={0},config[256]; GError *error=NULL;
 g_assert_false(goodix521d_configure(otp,config,&error));g_clear_error(&error);
 otp[30]=255;
 g_assert_false(goodix521d_configure(otp,config,&error));g_clear_error(&error);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);g_test_add_func("/goodix/private/exact",exact_private);g_test_add_func("/goodix/factory/reject",factory_invalid);return g_test_run();}
