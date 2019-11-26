#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>

//
//  for windows
//
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#endif

#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#if defined(WIN32)||defined(_WIN32)||defined(WINNT)||defined(__WINNT)
#define mkdir(_dir, _mode) mkdir(_dir)
#endif

#pragma pack(1)

//
//  文件头部结构
//  只关心文件内容，不关心文件属性
//
typedef struct _file_item{
        unsigned char  type;
        unsigned char  deep;
        unsigned short name_length;
        unsigned int   size;
        char name[0];
} file_item;

#pragma pack()

//
//  使用全局变量掌控全局
//
static int           g_infd = -1;   // 压缩文件
static int           g_outfd = -1;  // 解压缩文件
static char          g_buf[4096];   // 读写数据缓冲
static char          g_file_name[4096];  // 文件名缓冲,4096为文件名最大长度
static unsigned char g_random_key;  // 用于加密文件的一个简单KEY

// 当前只关心这两个类型
static const unsigned char g_type_file = 1; // 文件
static const unsigned char g_type_dir  = 2; // 目录

// 特殊声明
static int unpack( unsigned char deep );

// 判断是否一个正规文件
static int is_file( char * path )
{
    struct stat path_stat;
    int ret = stat(path, &path_stat);
    if( ret < 0 )
    {
        return 0;
    }
    return S_ISREG(path_stat.st_mode);
}

// 判断是否一个目录
static int is_dir( char * path )
{
    struct stat path_stat;
    int ret = stat(path, &path_stat);
    if( ret < 0 )
    {
        return 0;
    }
    return S_ISDIR(path_stat.st_mode);
}

// 获取文件大小
static ssize_t file_size( char * file )
{
    struct stat file_stat;
    int ret = stat(file, &file_stat);
    if( ret < 0 )
    {
        return -1;
    }
    return file_stat.st_size;
}

// 读取文件数据，并自动解密
static ssize_t read_in_file( void * buf, unsigned int len )
{
    ssize_t rret;
    rret = read( g_infd, buf, len );
    for(int i=0; i<len; i++ )
    {
        ((char*)buf)[i] ^= (char)(g_random_key*0xF3+0x05);
        g_random_key++;
    }
    return rret;
}

// 写入文件数据，并自动加密
static ssize_t write_out_file( void * buf, unsigned int len )
{
    ssize_t wret;
    char * out_buf = (char*)malloc(len);
    if( NULL == out_buf )
    {
        return ENOMEM;
    }
    for( int i=0; i<len; i++ )
    {
        out_buf[i] = ((char*)buf)[i]^(char)(g_random_key*0xF3+0x05);
        g_random_key++;
    }
    wret = write( g_outfd, out_buf, len );

    free(out_buf);
    return wret;
}

// 打包正规文件
static int pack_file( char * file_path, unsigned char deep )
{
    ssize_t rret;
    ssize_t wret;
    ssize_t fsize;
    ssize_t wsize;
    file_item item;
    int fd;

    fsize = file_size(file_path);
    if( fsize < 0 )
    {
        printf("ERROR: Failed to query file size, err=%d, file=%s\n", errno, file_path );
        return errno;
    }

    item.type = g_type_file;
    item.deep = deep;
    item.name_length = strlen(file_path);
    item.size = fsize;

    // 保存文件项
    wret = write_out_file( (char*)&item, sizeof(item) );
    if( wret < 0 )
    {
        printf("ERROR: Failed to save file item, err=%d\n", errno );
        return errno;
    }

    // 保存文件名
    wret = write_out_file( file_path, item.name_length );
    if( wret < 0 )
    {
        printf("ERROR: Failed to save file name[%s] to package, err=%d\n", file_path, errno );
        return errno;
    }

    fd = open( file_path, O_RDONLY|O_BINARY );
    if( fd < 0 )
    {
        printf("ERROR: Failed to open file, err=%d, file=%s\n", errno, file_path);
        return errno;
    }

    wsize = 0;
    do
    {
        rret = read( fd, g_buf, sizeof(g_buf));
        if( rret < 0 )
        {
            printf("ERROR: Failed to read file data, err=%d, file=%s\n", errno, file_path );
            break;
        }
        if( 0 == rret )
        {
            // no more data
            break;
        }
        wret = write_out_file( g_buf, rret );
        if( wret != rret )
        {
            printf("ERROR: Failed to save file data to package, err=%d\n", errno );
            break;
        }
        wsize += wret;

    }while( 0 < rret );

    close(fd);

    if( fsize != wsize )
    {
        printf("ERROR: Failed to save file to package, %d != %d\n", (int)fsize, (int)wsize);
        return EINVAL;
    }

    for( int i=0;i<deep; i++ )
    {
        printf("    ");
    }
    printf("%s: %d\n", file_path, (int)fsize);

    return 0;
}

// 打包目录
static int pack_dir( char * dir_path, unsigned char deep )
{
    int ret;
    DIR * dir;
    struct dirent * dent;
    ssize_t wret;
    file_item item;

    dir = opendir( dir_path );
    if( NULL == dir )
    {
        printf("ERROR: Failed to open dir, err=%d, dir=%s\n", errno, dir_path);
        return errno;
    }

    item.type        = g_type_dir;
    item.deep        = deep;
    item.name_length = strlen(dir_path);
    item.size        = 0;

    // 保存文件项
    wret = write_out_file( (char*)&item, sizeof(item) );
    if( wret < 0 )
    {
        printf("ERROR: Failed to save file item, err=%d\n", errno );
        return errno;
    }

    // 保存文件名
    wret = write_out_file( dir_path, item.name_length );
    if( wret < 0 )
    {
        printf("ERROR: Failed to save file name[%s] to package, err=%d\n", dir_path, errno );
        return errno;
    }

    for( int i=0;i<deep; i++ )
    {
        printf("    ");
    }
    printf("%s:\n", dir_path);

    ret = 0;
    deep++;
    chdir(dir_path);
    while( (dent=readdir(dir))!=NULL )
    {
        char * file_name = dent->d_name;
        if( 0 == strcmp(file_name, ".") ||
            0 == strcmp(file_name, "..") )
        {
            continue;
        }

        if( is_dir(file_name) )
        {
            ret = pack_dir(file_name, deep);
            if( 0 != ret )
            {
                break;
            }
        }
        else if( is_file(file_name) )
        {
            ret = pack_file(file_name, deep);
            if( 0 != ret )
            {
                break;
            }
        }
    }
    chdir("..");
    return ret;
}

// 打包模块初始化
static int pack_init( const char * package_file )
{
    ssize_t wret;

    g_outfd = open( package_file, O_CREAT|O_WRONLY|O_TRUNC|O_BINARY, 0644 );
    if( g_outfd < 0 )
    {
        printf("ERROR: Failed to create %s, err=%d\n", package_file, errno);
        return errno;
    }

    srand(time(NULL));
    g_random_key = (rand()+0x1D)*0x1D;
    wret = write( g_outfd, &g_random_key, sizeof(g_random_key));
    if( wret < 0 )
    {
        printf("ERROR: Failed to write file, err=%d, file=%s\n", errno, package_file);
        return errno;
    }

    return 0;
}

// 打包模块反初始化
static int pack_fini()
{
    if( g_outfd < 0 )
    {
        return EINVAL;
    }
    close(g_outfd);
    return 0;
}

// 打包
static int do_pack( int argc, char * argv[] )
{
    int ret;
    char * path;

    ret = pack_init( argv[0] );
    if( 0!= ret )
    {
        return ret;
    }
    for( int i=1; i<argc; i++ )
    {
        path = argv[i];
        if( is_file(path) )
        {
            ret = pack_file( path, 0 );
        }
        else if( is_dir(path) )
        {
            ret = pack_dir( path, 0 );
        }
        else
        {
            ret = 0;
        }
        if( 0 != ret )
        {
            printf("ERROR: Failed to package %s\n", path );
            break;
        }
    }
    pack_fini();
    return ret;
}

// 解包文件中的正规文件
static int unpack_file( file_item * item, unsigned char deep )
{
    int ret;
    int fd;
    ssize_t rret;
    ssize_t wret;
    char * name = g_file_name;
    name[item->name_length] = '\0';
    rret = read_in_file( name, item->name_length );
    if( rret < 0 )
    {
        printf("ERROR: Faile to read file name, err=%d\n", errno);
        return errno;
    }
    if( strlen(name) != item->name_length )
    {
        printf("ERROR: %d != %d, deep:%d\n", (int)strlen(name), item->name_length, item->deep);
        return EINVAL;
    }

    for( int i=0;i<item->deep; i++ )
    {
        printf("    ");
    }
    printf("%s: %d\n", name, item->size);

    fd = open( name, O_CREAT|O_WRONLY|O_TRUNC|O_BINARY, 0644 );
    if( fd < 0 )
    {
        printf("ERROR: Failed to create filem err=%d, file=%s\n", errno, name);
        return errno;
    }

    ret = 0;
    for( int i=0; i<item->size; i+=rret )
    {
        int byte = sizeof(g_buf);
        if( item->size-i < byte )
        {
            byte = item->size-i;
        }
        rret = read_in_file( g_buf, byte );
        if( rret < 0 )
        {
            printf("ERROR: Faile to read file, err=%d, file=%s\n", errno, name);
            ret = errno;
            break;
        }
        wret = write( fd, g_buf, rret );
        if( wret < 0 )
        {
            printf("ERROR: Faile to write file, err=%d, file=:%s\n", errno, name);
            ret = errno;
            break;
        }
    }
    close(fd);
    return ret;
}

// 解包文件中的目录
static int unpack_dir( file_item * item, unsigned char deep )
{
    int ret;
    ssize_t rret;
    char * name = malloc(item->name_length+1);
    name[item->name_length] = '\0';

    rret = read_in_file( name, item->name_length );
    if( rret < 0 )
    {
        printf("ERROR: Faile to read file name, deep:%d\n", item->deep);
        return errno;
    }

    for( int i=0;i<item->deep; i++ )
    {
        printf("    ");
    }
    printf("%s:\n", name);

    ret = mkdir( name, 0755 );
    if( ret < 0 )
    {
        printf("ERROR: Faile to create dir, err=%d, dir=%s\n", errno, name);
        return errno;
    }

    chdir( name );
    ret = unpack(deep+1);
    chdir( ".." );
    return ret;
}

// 解包数据
static int unpack( unsigned char deep )
{
    int ret;
    file_item item;
    ssize_t rret;

    ret = 0;
    do
    {
        rret = read_in_file( &item, sizeof(item) );
        if( rret < 0 )
        {
            printf("ERROR: Failed to read data from package file, err=%d\n", errno);
            return errno;
        }
        if( rret == 0 )
        {
            break;
        }

        if( item.deep < deep )
        {
            // 文件不属于整个目录， 返回上一级， 设置一些文件偏移
            lseek( g_infd, 0-sizeof(item), SEEK_CUR );
            g_random_key -= sizeof(item);
            return 0;
        }

        if( g_type_file == item.type ) // 释放文件
        {
            ret = unpack_file(&item, deep);
        }
        else if( g_type_dir == item.type ) // 释放目录
        {
            ret = unpack_dir(&item, deep);
        }
        else // 未知类型，报个错
        {
            printf("ERROR: Failed to unpackage file, type=%d\n", item.type );
            ret = EINVAL;
        }

        if( 0 != ret )
        {
            break;
        }

    }while( 1 );

    return ret;
}

// 解包模块初始化
static int unpack_init( const char * package_file, const char * release_dir )
{
    ssize_t rret;

    g_infd = open( package_file, O_RDONLY|O_BINARY );
    if( g_infd < 0 )
    {
        printf("ERROR: Failed to open %s, err=%d\n", package_file, errno);
        return errno;
    }

    rret = read( g_infd, &g_random_key, sizeof(g_random_key) );
    if( rret < 0 )
    {
        printf("ERROR: Failed to read file, err=%d, file=%s\n", errno, package_file );
        return errno;
    }

    mkdir(release_dir, 0755);
    chdir(release_dir);
    return 0;
}

// 解包模块反初始化
static int unpack_fini()
{
    if( g_infd < 0 )
    {
        return EINVAL;
    }
    close(g_infd);
    return 0;
}

// 解包
static int do_unpack( int argc, char * argv[] )
{
    int ret;
    ret = unpack_init( argv[0], argv[1] );
    if( 0 != ret )
    {
        return ret;
    }
    ret = unpack(0);

    unpack_fini();

    return ret;
}

// help
static int usage( const char * program )
{
    printf("Usage:\r\n");
    printf("    %s -pack <package file name> <file1|dir1> [<file2|dir2>] ...\n", program );
    printf("    %s -unpack <package file name> <dir name>\n", program );
    return EINVAL;
}

// hello
int main( int argc, char *argv[] )
{
    if( argc < 4 )
    {
        return usage( argv[0] );
    }

    if( 0 == strcmp(argv[1], "-pack") )
    {
        return do_pack(argc-2, argv+2);
    }
    else if( 0 == strcmp(argv[1], "-unpack") )
    {
        return do_unpack(argc-2, argv+2);
    }

    return usage( argv[0] );
}

