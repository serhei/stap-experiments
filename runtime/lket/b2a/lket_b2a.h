#ifndef _LKET_B2A_H
#define _LKET_B2A_H
#include <glib.h>

#define LKET_MAGIC	0xAEFCDB6B

#define MAX_STRINGLEN	256
#define MAX_HOOKGROUP	255
#define MAX_HOOKID	255

#define APPNAMELIST_LEN	256

#define SEQID_SIZE 4

#define LKET_PKT_SYS 1
#define LKET_PKT_BT 2
#define LKET_PKT_USER 3

#define DEFAULT_OUTFILE_NAME "lket.out"

typedef struct _lket_pkt_header {
	int8_t	flag;
	int16_t	size;
	int8_t	hookgroup;
	int8_t	hookid;
	int32_t sec;
	int32_t usec;
	int32_t	pid;
	int32_t	ppid;
	int32_t	tid;
	int8_t	cpu;
} __attribute__((packed)) lket_pkt_header;

typedef struct _appname_info {
	int pid;
	int ppid;
	int tid;
	long index;
	struct _appname_info *next;
} appname_info;

typedef struct _hook_fmt {
	int hookgrp;
	int hookid;
	const char *fmt;
} hook_fmt;

/*
 * register one hookdata fmt string for a [hookgroup, hookid] pair
 */
static int register_one_fmt(int hookgroup, int hookid, const char *fmt, size_t maxlen);

/*
 * initialize all the hookdata fmt strings as required
 * called at the beginning of main()
 */
static void register_formats(void);

/*
 * get the format string with [hookgroup, hookid] pair
 */
static const char *get_fmt(int hookgroup, int hookid);

/* 
 * handle the bothering sequence id generated by systemtap
 */
static int skip_sequence_id(FILE *fp);

/* 
 * search the lket_init_header structure in a set of input files 
 */
static void find_init_header(FILE **fp, const int total_infiles, FILE *outfp);

/* 
 * read the lket_pkt_header structure at the begining of the input file 
 */
static int get_pkt_header(FILE *fp, lket_pkt_header *phdr);

/* 
 * print the lket_pkt_header structure into the output file
 */
static void print_pkt_header(FILE *fp, lket_pkt_header *phdr);

/*
 * Get the appropriate appname index based on the [pid, ppid, tid] triple
 * If exists, return it; otherwise, assigned a new one.
 */
//static long appname_index(int pid, int ppid, int tid);

/* 
 * read fixed-length from the input binary file and write into 
 * the output file, based on the fmt string
 */
static void b2a_vsnprintf(const char *fmt, FILE *infp, FILE *outfile, size_t size);

void register_appname(int i, FILE *fp, lket_pkt_header *phdr);
gint compareFunc(gconstpointer a, gconstpointer b, gpointer user_data);
void destroyAppName(gpointer data);

#endif
