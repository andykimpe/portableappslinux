/*
* Configuration parameters shared between Wine server and clients
*
* Copyright 2002 Alexandre Julliard
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include "config.h"
#include "wine/port.h"
 
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
 30 #ifdef HAVE_UNISTD_H
 31 # include <unistd.h>
 32 #endif
 33 #ifdef HAVE_PWD_H
 34 #include <pwd.h>
 35 #endif
 36 #include "wine/library.h"
 37 
 38 static const char server_config_dir[] = "/.wine";        /* config dir relative to $HOME */
 39 static const char server_root_prefix[] = "/tmp/.wine";   /* prefix for server root dir */
 40 static const char server_dir_prefix[] = "/server-";      /* prefix for server dir */
 41 
 42 static char *bindir;
 43 static char *dlldir;
 44 static char *datadir;
 45 static char *config_dir;
 46 static char *server_dir;
 47 static char *build_dir;
 48 static char *user_name;
 49 static char *argv0_name;
 50 
 51 #ifdef __GNUC__
 52 static void fatal_error( const char *err, ... )  __attribute__((noreturn,format(printf,1,2)));
 53 static void fatal_perror( const char *err, ... )  __attribute__((noreturn,format(printf,1,2)));
 54 #endif
 55 
 56 #if defined(__linux__) || defined(__FreeBSD_kernel__ )
 57 #define EXE_LINK "/proc/self/exe"
 58 #elif defined (__FreeBSD__) || defined(__DragonFly__)
 59 #define EXE_LINK "/proc/curproc/file"
 60 #endif
 61 
 62 /* die on a fatal error */
 63 static void fatal_error( const char *err, ... )
 64 {
 65     va_list args;
 66 
 67     va_start( args, err );
 68     fprintf( stderr, "wine: " );
 69     vfprintf( stderr, err, args );
 70     va_end( args );
 71     exit(1);
 72 }
 73 
 74 /* die on a fatal error */
 75 static void fatal_perror( const char *err, ... )
 76 {
 77     va_list args;
 78 
 79     va_start( args, err );
 80     fprintf( stderr, "wine: " );
 81     vfprintf( stderr, err, args );
 82     perror( " " );
 83     va_end( args );
 84     exit(1);
 85 }
 86 
 87 /* malloc wrapper */
 88 static void *xmalloc( size_t size )
 89 {
 90     void *res;
 91 
 92     if (!size) size = 1;
 93     if (!(res = malloc( size ))) fatal_error( "virtual memory exhausted\n");
 94     return res;
 95 }
 96 
 97 /* strdup wrapper */
 98 static char *xstrdup( const char *str )
 99 {
100     size_t len = strlen(str) + 1;
101     char *res = xmalloc( len );
102     memcpy( res, str, len );
103     return res;
104 }
105 
106 /* check if a string ends in a given substring */
107 static inline int strendswith( const char* str, const char* end )
108 {
109     size_t len = strlen( str );
110     size_t tail = strlen( end );
111     return len >= tail && !strcmp( str + len - tail, end );
112 }
113 
114 /* remove all trailing slashes from a path name */
115 static inline void remove_trailing_slashes( char *path )
116 {
117     int len = strlen( path );
118     while (len > 1 && path[len-1] == '/') path[--len] = 0;
119 }
120 
121 /* build a path from the specified dir and name */
122 static char *build_path( const char *dir, const char *name )
123 {
124     size_t len = strlen(dir);
125     char *ret = xmalloc( len + strlen(name) + 2 );
126 
127     memcpy( ret, dir, len );
128     if (len && ret[len-1] != '/') ret[len++] = '/';
129     strcpy( ret + len, name );
130     return ret;
131 }
132 
133 /* return the directory that contains the library at run-time */
134 static char *get_runtime_libdir(void)
135 {
136 #ifdef HAVE_DLADDR
137     Dl_info info;
138     char *libdir;
139 
140     if (dladdr( get_runtime_libdir, &info ) && info.dli_fname[0] == '/')
141     {
142         const char *p = strrchr( info.dli_fname, '/' );
143         unsigned int len = p - info.dli_fname;
144         if (!len) len++;  /* include initial slash */
145         libdir = xmalloc( len + 1 );
146         memcpy( libdir, info.dli_fname, len );
147         libdir[len] = 0;
148         return libdir;
149     }
150 #endif /* HAVE_DLADDR */
151     return NULL;
152 }
153 
154 /* return the directory that contains the main exe at run-time */
155 static char *get_runtime_exedir(void)
156 {
157 #ifdef EXE_LINK
158     char *p, *bindir;
159     int size;
160 
161     for (size = 256; ; size *= 2)
162     {
163         int ret;
164         if (!(bindir = malloc( size ))) return NULL;
165         if ((ret = readlink( EXE_LINK, bindir, size )) == -1) break;
166         if (ret != size)
167         {
168             bindir[ret] = 0;
169             if (!(p = strrchr( bindir, '/' ))) break;
170             if (p == bindir) p++;
171             *p = 0;
172             return bindir;
173         }
174         free( bindir );
175     }
176     free( bindir );
177 #endif
178     return NULL;
179 }
180 
181 /* return the base directory from argv0 */
182 static char *get_runtime_argvdir( const char *argv0 )
183 {
184     char *p, *bindir, *cwd;
185     int len, size;
186 
187     if (!(p = strrchr( argv0, '/' ))) return NULL;
188 
189     len = p - argv0;
190     if (!len) len++;  /* include leading slash */
191 
192     if (argv0[0] == '/')  /* absolute path */
193     {
194         bindir = xmalloc( len + 1 );
195         memcpy( bindir, argv0, len );
196         bindir[len] = 0;
197     }
198     else
199     {
200         /* relative path, make it absolute */
201         for (size = 256 + len; ; size *= 2)
202         {
203             if (!(cwd = malloc( size ))) return NULL;
204             if (getcwd( cwd, size - len ))
205             {
206                 bindir = cwd;
207                 cwd += strlen(cwd);
208                 *cwd++ = '/';
209                 memcpy( cwd, argv0, len );
210                 cwd[len] = 0;
211                 break;
212             }
213             free( cwd );
214             if (errno != ERANGE) return NULL;
215         }
216     }
217     return bindir;
218 }
219 
220 /* initialize the server directory value */
221 static void init_server_dir( dev_t dev, ino_t ino )
222 {
223     char *p, *root;
224 
225 #ifdef __ANDROID__  /* there's no /tmp dir on Android */
226     root = build_path( config_dir, ".wineserver" );
227 #elif defined(HAVE_GETUID)
228     root = xmalloc( sizeof(server_root_prefix) + 12 );
229     sprintf( root, "%s-%u", server_root_prefix, getuid() );
230 #else
231     root = xstrdup( server_root_prefix );
232 #endif
233 
234     server_dir = xmalloc( strlen(root) + sizeof(server_dir_prefix) + 2*sizeof(dev) + 2*sizeof(ino) + 2 );
235     strcpy( server_dir, root );
236     strcat( server_dir, server_dir_prefix );
237     p = server_dir + strlen(server_dir);
238 
239     if (dev != (unsigned long)dev)
240         p += sprintf( p, "%lx%08lx-", (unsigned long)((unsigned long long)dev >> 32), (unsigned long)dev );
241     else
242         p += sprintf( p, "%lx-", (unsigned long)dev );
243 
244     if (ino != (unsigned long)ino)
245         sprintf( p, "%lx%08lx", (unsigned long)((unsigned long long)ino >> 32), (unsigned long)ino );
246     else
247         sprintf( p, "%lx", (unsigned long)ino );
248     free( root );
249 }
250 
251 /* retrieve the default dll dir */
252 const char *get_dlldir( const char **default_dlldir, const char **dll_prefix )
253 {
254     *default_dlldir = DLLDIR;
255     *dll_prefix = "/" DLLPREFIX;
256     return dlldir;
257 }
258 
259 /* initialize all the paths values */
260 static void init_paths(void)
261 {
262     struct stat st;
263 
264     const char *home = getenv( "HOME" );
265     const char *user = NULL;
266     const char *prefix = getenv( "WINEPREFIX" );
267 
268 #ifdef HAVE_GETPWUID
269     char uid_str[32];
270     struct passwd *pwd = getpwuid( getuid() );
271 
272     if (pwd)
273     {
274         user = pwd->pw_name;
275         if (!home) home = pwd->pw_dir;
276     }
277     if (!user)
278     {
279         sprintf( uid_str, "%lu", (unsigned long)getuid() );
280         user = uid_str;
281     }
282 #else  /* HAVE_GETPWUID */
283     if (!(user = getenv( "USER" )))
284         fatal_error( "cannot determine your user name, set the USER environment variable\n" );
285 #endif  /* HAVE_GETPWUID */
286     user_name = xstrdup( user );
287 
288     /* build config_dir */
289 
290     if (prefix)
291     {
292         config_dir = xstrdup( prefix );
293         remove_trailing_slashes( config_dir );
294         if (config_dir[0] != '/')
295             fatal_error( "invalid directory %s in WINEPREFIX: not an absolute path\n", prefix );
296         if (stat( config_dir, &st ) == -1)
297         {
298             if (errno == ENOENT) return;  /* will be created later on */
299             fatal_perror( "cannot open %s as specified in WINEPREFIX", config_dir );
300         }
301     }
302     else
303     {
304         if (!home) fatal_error( "could not determine your home directory\n" );
305         if (home[0] != '/') fatal_error( "your home directory %s is not an absolute path\n", home );
306         config_dir = xmalloc( strlen(home) + sizeof(server_config_dir) );
307         strcpy( config_dir, home );
308         remove_trailing_slashes( config_dir );
309         strcat( config_dir, server_config_dir );
310         if (stat( config_dir, &st ) == -1)
311         {
312             if (errno == ENOENT) return;  /* will be created later on */
313             fatal_perror( "cannot open %s", config_dir );
314         }
315     }
316     if (!S_ISDIR(st.st_mode)) fatal_error( "%s is not a directory\n", config_dir );
317 #ifdef HAVE_GETUID
318     if (st.st_uid != getuid()) fatal_error( "%s is not owned by you\n", config_dir );
319 #endif
320 
321     init_server_dir( st.st_dev, st.st_ino );
322 }
323 
324 /* check if bindir is valid by checking for wineserver */
325 static int is_valid_bindir( const char *bindir )
326 {
327     struct stat st;
328     char *path = build_path( bindir, "wineserver" );
329     int ret = (stat( path, &st ) != -1);
330     free( path );
331     return ret;
332 }
333 
334 /* check if basedir is a valid build dir by checking for wineserver and ntdll */
335 /* helper for running_from_build_dir */
336 static inline int is_valid_build_dir( char *basedir, int baselen )
337 {
338     struct stat st;
339 
340     strcpy( basedir + baselen, "/server/wineserver" );
341     if (stat( basedir, &st ) == -1) return 0;  /* no wineserver found */
342     /* check for ntdll too to make sure */
343     strcpy( basedir + baselen, "/dlls/ntdll/ntdll.dll.so" );
344     if (stat( basedir, &st ) == -1) return 0;  /* no ntdll found */
345 
346     basedir[baselen] = 0;
347     return 1;
348 }
349 
350 /* check if we are running from the build directory */
351 static char *running_from_build_dir( const char *basedir )
352 {
353     const char *p;
354     char *path;
355 
356     /* remove last component from basedir */
357     p = basedir + strlen(basedir) - 1;
358     while (p > basedir && *p == '/') p--;
359     while (p > basedir && *p != '/') p--;
360     if (p == basedir) return NULL;
361     path = xmalloc( p - basedir + sizeof("/dlls/ntdll/ntdll.dll.so") );
362     memcpy( path, basedir, p - basedir );
363 
364     if (!is_valid_build_dir( path, p - basedir ))
365     {
366         /* remove another component */
367         while (p > basedir && *p == '/') p--;
368         while (p > basedir && *p != '/') p--;
369         if (p == basedir || !is_valid_build_dir( path, p - basedir ))
370         {
371             free( path );
372             return NULL;
373         }
374     }
375     return path;
376 }
377 
378 /* initialize the argv0 path */
379 void wine_init_argv0_path( const char *argv0 )
380 {
381     const char *basename;
382     char *libdir;
383 
384     if (!(basename = strrchr( argv0, '/' ))) basename = argv0;
385     else basename++;
386 
387     bindir = get_runtime_exedir();
388     if (bindir && !is_valid_bindir( bindir ))
389     {
390         build_dir = running_from_build_dir( bindir );
391         free( bindir );
392         bindir = NULL;
393     }
394 
395     libdir = get_runtime_libdir();
396     if (libdir && !bindir && !build_dir)
397     {
398         build_dir = running_from_build_dir( libdir );
399         if (!build_dir) bindir = build_path( libdir, LIB_TO_BINDIR );
400     }
401 
402     if (!libdir && !bindir && !build_dir)
403     {
404         bindir = get_runtime_argvdir( argv0 );
405         if (bindir && !is_valid_bindir( bindir ))
406         {
407             build_dir = running_from_build_dir( bindir );
408             free( bindir );
409             bindir = NULL;
410         }
411     }
412 
413     if (build_dir)
414     {
415         argv0_name = build_path( "loader/", basename );
416     }
417     else
418     {
419         if (libdir) dlldir = build_path( libdir, LIB_TO_DLLDIR );
420         else if (bindir) dlldir = build_path( bindir, BIN_TO_DLLDIR );
421 
422         if (bindir) datadir = build_path( bindir, BIN_TO_DATADIR );
423         argv0_name = xstrdup( basename );
424     }
425     free( libdir );
426 }
427 
428 /* return the configuration directory ($WINEPREFIX or $HOME/.wine) */
429 const char *wine_get_config_dir(void)
430 {
431     if (!config_dir) init_paths();
432     return config_dir;
433 }
434 
435 /* retrieve the wine data dir */
436 const char *wine_get_data_dir(void)
437 {
438     return datadir;
439 }
440 
441 /* retrieve the wine build dir (if we are running from there) */
442 const char *wine_get_build_dir(void)
443 {
444     return build_dir;
445 }
446 
447 /* return the full name of the server directory (the one containing the socket) */
448 const char *wine_get_server_dir(void)
449 {
450     if (!server_dir)
451     {
452         if (!config_dir) init_paths();
453         else
454         {
455             struct stat st;
456 
457             if (stat( config_dir, &st ) == -1)
458             {
459                 if (errno != ENOENT) fatal_error( "cannot stat %s\n", config_dir );
460                 return NULL;  /* will have to try again once config_dir has been created */
461             }
462             init_server_dir( st.st_dev, st.st_ino );
463         }
464     }
465     return server_dir;
466 }
467 
468 /* return the current user name */
469 const char *wine_get_user_name(void)
470 {
471     if (!user_name) init_paths();
472     return user_name;
473 }
474 
475 /* return the standard version string */
476 const char *wine_get_version(void)
477 {
478     return PACKAGE_VERSION;
479 }
480 
481 /* return the build id string */
482 const char *wine_get_build_id(void)
483 {
484     extern const char wine_build[];
485     return wine_build;
486 }
487 
488 /* exec a binary using the preloader if requested; helper for wine_exec_wine_binary */
489 static void preloader_exec( char **argv, int use_preloader )
490 {
491     if (use_preloader)
492     {
493         static const char preloader[] = "wine-preloader";
494         static const char preloader64[] = "wine64-preloader";
495         char *p, *full_name;
496         char **last_arg = argv, **new_argv;
497 
498         if (!(p = strrchr( argv[0], '/' ))) p = argv[0];
499         else p++;
500 
501         full_name = xmalloc( p - argv[0] + sizeof(preloader64) );
502         memcpy( full_name, argv[0], p - argv[0] );
503         if (strendswith( p, "64" ))
504             memcpy( full_name + (p - argv[0]), preloader64, sizeof(preloader64) );
505         else
506             memcpy( full_name + (p - argv[0]), preloader, sizeof(preloader) );
507 
508         /* make a copy of argv */
509         while (*last_arg) last_arg++;
510         new_argv = xmalloc( (last_arg - argv + 2) * sizeof(*argv) );
511         memcpy( new_argv + 1, argv, (last_arg - argv + 1) * sizeof(*argv) );
512         new_argv[0] = full_name;
513         execv( full_name, new_argv );
514         free( new_argv );
515         free( full_name );
516     }
517     execv( argv[0], argv );
518 }
519 
520 /* exec a wine internal binary (either the wine loader or the wine server) */
521 void wine_exec_wine_binary( const char *name, char **argv, const char *env_var )
522 {
523     const char *path, *pos, *ptr;
524     int use_preloader;
525 
526     if (!name) name = argv0_name;  /* no name means default loader */
527 
528 #ifdef linux
529     use_preloader = !strendswith( name, "wineserver" );
530 #else
531     use_preloader = 0;
532 #endif
533 
534     if ((ptr = strrchr( name, '/' )))
535     {
536         /* if we are in build dir and name contains a path, try that */
537         if (build_dir)
538         {
539             argv[0] = build_path( build_dir, name );
540             preloader_exec( argv, use_preloader );
541             free( argv[0] );
542         }
543         name = ptr + 1;  /* get rid of path */
544     }
545 
546     /* first, bin directory from the current libdir or argv0 */
547     if (bindir)
548     {
549         argv[0] = build_path( bindir, name );
550         preloader_exec( argv, use_preloader );
551         free( argv[0] );
552     }
553 
554     /* then specified environment variable */
555     if (env_var)
556     {
557         argv[0] = (char *)env_var;
558         preloader_exec( argv, use_preloader );
559     }
560 
561     /* now search in the Unix path */
562     if ((path = getenv( "PATH" )))
563     {
564         argv[0] = xmalloc( strlen(path) + strlen(name) + 2 );
565         pos = path;
566         for (;;)
567         {
568             while (*pos == ':') pos++;
569             if (!*pos) break;
570             if (!(ptr = strchr( pos, ':' ))) ptr = pos + strlen(pos);
571             memcpy( argv[0], pos, ptr - pos );
572             strcpy( argv[0] + (ptr - pos), "/" );
573             strcat( argv[0] + (ptr - pos), name );
574             preloader_exec( argv, use_preloader );
575             pos = ptr;
576         }
577         free( argv[0] );
578     }
579 
580     /* and finally try BINDIR */
581     argv[0] = build_path( BINDIR, name );
582     preloader_exec( argv, use_preloader );
583     free( argv[0] );
584 }
585 
