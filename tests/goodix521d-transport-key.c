/* Synthetic metadata and key bytes only. No actual key file is opened. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "goodixtls.h"
extern gboolean goodix_tls_load_psk_file(const char *, guint8[32], GError **)
  __attribute__((weak));
static unsigned opens, closes;
static int open_errno, stat_errno;
static mode_t mode;
static uid_t uid;
static off_t size;
static ssize_t read_size;
static const char *expected_path;
int __wrap_open(const char *path, int flags, ...) {
 assert(!strcmp(path,expected_path)); opens++;
 assert((flags & (O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC))==(O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
 assert((flags & O_ACCMODE)==O_RDONLY);
 if(open_errno) {errno=open_errno;return -1;} return 42;
}
int __wrap_fstat(int fd, struct stat *st) {
 assert(fd==42);memset(st,0,sizeof *st);
 if(stat_errno) {errno=stat_errno;return -1;}
 st->st_mode=mode;st->st_uid=uid;st->st_size=size;return 0;
}
ssize_t __wrap_read(int fd, void *data, size_t length) {
 assert(fd==42);assert(length==33);if(read_size<0){errno=EIO;return -1;}
 memset(data,0xa5,(size_t)read_size);return read_size;
}
int __wrap_close(int fd) {assert(fd==42);closes++;return 0;}
static void reset(void) {
 opens=closes=0;open_errno=stat_errno=0;mode=S_IFREG|0600;uid=0;size=read_size=32;
 expected_path="/private/reader.psk";
}
static void rejected(const char *path) {
 guint8 key[32];memset(key,0xcc,sizeof key);GError *e=NULL;
 assert(!goodix_tls_load_psk_file(path,key,&e));assert(e);
 for(unsigned i=0;i<sizeof key;i++)assert(key[i]==0);
 g_clear_error(&e);
}
int main(void) {
 if(!goodix_tls_load_psk_file){fputs("FAIL: explicit validated PSK path API is missing\n",stderr);return 1;}
 reset();rejected(NULL);assert(opens==0);rejected("");assert(opens==0);rejected("reader.psk");assert(opens==0);
 reset();open_errno=ENOENT;rejected(expected_path);assert(opens==1&&closes==0);
 reset();open_errno=ELOOP;rejected(expected_path);assert(opens==1&&closes==0);
 reset();stat_errno=EIO;rejected(expected_path);assert(closes==1);
 reset();mode=S_IFIFO|0600;rejected(expected_path);assert(closes==1);
 reset();mode=S_IFREG|0640;rejected(expected_path);assert(closes==1);
 reset();uid=1000;rejected(expected_path);assert(closes==1);
 reset();size=31;rejected(expected_path);assert(closes==1);
 reset();size=33;rejected(expected_path);assert(closes==1);
 reset();read_size=31;rejected(expected_path);assert(closes==1);
 reset();read_size=33;rejected(expected_path);assert(closes==1);
 reset();read_size=-1;rejected(expected_path);assert(closes==1);
 reset();guint8 key[32];GError *e=NULL;assert(goodix_tls_load_psk_file(expected_path,key,&e));assert(!e&&opens==1&&closes==1);
 for(unsigned i=0;i<sizeof key;i++)assert(key[i]==0xa5);
 reset();expected_path="/etc/libfprint/goodix-521d.psk";open_errno=ENOENT;
 assert(!goodix_tls_load_521d_psk(key,&e));assert(e&&opens==1&&closes==0);g_clear_error(&e);
 for(unsigned i=0;i<sizeof key;i++)assert(key[i]==0);
 puts("PASS: explicit key path, root-private metadata, exact length and no fallback");return 0;
}
