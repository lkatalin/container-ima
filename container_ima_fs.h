/*
 * container_ima_fs.c
 *      Security file system for container measurment lists       
 *
 */
#ifndef __CONTAINER_IMA_FS_H__
#define __CONTAINER_IMA_FS_H__

#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/parser.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/integrity.h>
#include <linux/ima.h>
#include <asm/mman.h>
#include <linux/mman.h>

#include "container_ima_init.h"
#include "container_ima_fs.h"
#include "container_ima_api.h"
#include "container_ima.h"

static DEFINE_MUTEX(ima_write_mutex);

bool ima_canonical_fmt;

static int __init default_canonical_fmt_setup(char *str)
{
#ifdef __BIG_ENDIAN
	ima_canonical_fmt = true;
#endif
	return 1;
}
__setup("ima_canonical_fmt", default_canonical_fmt_setup);

static int valid_policy = 1;

void ima_print_digest(struct seq_file *m, u8 *digest, u32 size)
{
	u32 i;

	for (i = 0; i < size; i++)
		seq_printf(m, "%02x", *(digest + i));
}

static void *c_ima_measurements_next(struct seq_file *m, void *v, loff_t *pos)
{
    struct container_ima_data *data;
    struct ima_queue_entry *qe = v;

    data = ima_data_from_file(m->file);

    rcu_read_lock();
    qe = list_entry_rcu(qe->later.next, struct ima_queue_entry, later);
    rcu_read_unlock();
    (*pos)++;

    return (&qe->later == &data->c_ima_measurements) ? NULL : qe;
}
/* use default, adjust later if needed (probably needed) */
static const struct file_operations c_ima_measurements_ops = {
	.open = c_ima_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
}; 
void ima_putc(struct seq_file *m, void *data, int datalen)
{
	while (datalen--)
		seq_putc(m, *(char *)data++);
}
static int ima_ascii_measurements_show(struct seq_file *m, void *v)
{
	/* the list never shrinks, so we don't need a lock here */
	struct ima_queue_entry *qe = v;
	struct ima_template_entry *e;
	char *template_name;
	int i;

	/* get entry */
	e = qe->entry;
	if (e == NULL)
		return -1;

	template_name = (e->template_desc->name[0] != '\0') ?
	    e->template_desc->name : e->template_desc->fmt;

	/* 1st: PCR used (config option) */
	seq_printf(m, "%2d ", e->pcr);

	/* 2nd: SHA1 template hash */
	ima_print_digest(m, e->digest, TPM_DIGEST_SIZE);

	/* 3th:  template name */
	seq_printf(m, " %s", template_name);

	/* 4th:  template specific data */
	for (i = 0; i < e->template_desc->num_fields; i++) {
		seq_puts(m, " ");
		if (e->template_data[i].len == 0)
			continue;

		e->template_desc->fields[i]->field_show(m, IMA_SHOW_ASCII,
							&e->template_data[i]);
	}
	seq_puts(m, "\n");
	return 0;
}

/* returns pointer to hlist_node */
static void *ima_measurements_start(struct seq_file *m, loff_t *pos)
{
	loff_t l = *pos;
	struct ima_queue_entry *qe;
	unsigned int id;
	struct container_ima_data *data;
	struct task_struct *task = get_current();

	data = ima_data_exists(id);
	/* we need a lock since pos could point beyond last element */
	rcu_read_lock();
	list_for_each_entry_rcu(qe, &data->c_ima_measurements, later) {
		if (!l--) {
			rcu_read_unlock();
			return qe;
		}
	}
	rcu_read_unlock();
	return NULL;
}
int ima_measurements_show(struct seq_file *m, void *v)
{
	/* the list never shrinks, so we don't need a lock here */
	struct ima_queue_entry *qe = v;
	struct ima_template_entry *e;
	char *template_name;
	u32 pcr, namelen, template_data_len; /* temporary fields */
	bool is_ima_template = false;
	int i;

	/* get entry */
	e = qe->entry;
	if (e == NULL)
		return -1;

	template_name = (e->template_desc->name[0] != '\0') ?
	    e->template_desc->name : e->template_desc->fmt;

	/*
	 * 1st: PCRIndex
	 * PCR used defaults to the same (config option) in
	 * little-endian format, unless set in policy
	 */
	pcr = !ima_canonical_fmt ? e->pcr : cpu_to_le32(e->pcr);
	ima_putc(m, &pcr, sizeof(e->pcr));

	/* 2nd: template digest */
	ima_putc(m, e->digest, TPM_DIGEST_SIZE);

	/* 3rd: template name size */
	namelen = !ima_canonical_fmt ? strlen(template_name) :
		cpu_to_le32(strlen(template_name));
	ima_putc(m, &namelen, sizeof(namelen));

	/* 4th:  template name */
	ima_putc(m, template_name, strlen(template_name));

	/* 5th:  template length (except for 'ima' template) */
	if (strcmp(template_name, IMA_TEMPLATE_IMA_NAME) == 0)
		is_ima_template = true;

	if (!is_ima_template) {
		template_data_len = !ima_canonical_fmt ? e->template_data_len :
			cpu_to_le32(e->template_data_len);
		ima_putc(m, &template_data_len, sizeof(e->template_data_len));
	}

	/* 6th:  template specific data */
	for (i = 0; i < e->template_desc->num_fields; i++) {
		enum ima_show_type show = IMA_SHOW_BINARY;
		struct ima_template_field *field = e->template_desc->fields[i];

		if (is_ima_template && strcmp(field->field_id, "d") == 0)
			show = IMA_SHOW_BINARY_NO_FIELD_LEN;
		if (is_ima_template && strcmp(field->field_id, "n") == 0)
			show = IMA_SHOW_BINARY_OLD_STRING_FMT;
		field->field_show(m, show, &e->template_data[i]);
	}
	return 0;
}
static void ima_measurements_stop(struct seq_file *m, void *v)
{
}
/* operations for ascii file, try to use IMA's seq_ops*/
static const struct seq_operations c_ima_ascii_measurements_seqops = {
	.start = ima_measurements_start,
	.next = c_ima_measurements_next,
	.stop = ima_measurements_stop,
	.show = ima_ascii_measurements_show
};
static const struct seq_operations c_ima_measurments_seqops = {
	.start = ima_measurements_start,
	.next = c_ima_measurements_next,
	.stop = ima_measurements_stop,
	.show = ima_measurements_show
};

static int c_ima_seq_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &c_ima_measurments_seqops);
}
static ssize_t c_ima_show_htable_violations(struct file *filp,
					  char __user *buf,
					  size_t count, loff_t *ppos) 
{
    struct container_ima_data *data;
    char tmp[32];
    ssize_t len;
    atomic_long_t *val;

    data = ima_data_from_file(filp);
    val = &data->hash_tbl->violations;
    len = scnprintf(tmp, sizeof(tmp), "%li\n", atomic_long_read(val));

    return  simple_read_from_buffer(buf, count, ppos, tmp, len);
}
static ssize_t ima_show_htable_value(char __user *buf, size_t count,
				     loff_t *ppos, atomic_long_t *val)
{
	char tmpbuf[32];	/* greater than largest 'long' string value */
	ssize_t len;
	len = scnprintf(tmpbuf, sizeof(tmpbuf), "%li\n", atomic_long_read(val));
	return simple_read_from_buffer(buf, count, ppos, tmpbuf, len);
}

static ssize_t ima_show_htable_violations(struct file *filp,
					  char __user *buf,
					  size_t count, loff_t *ppos)
{
	unsigned int id;
	struct container_ima_data *data;
	struct task_struct *task = get_current();

	data = ima_data_exists(id);
	return ima_show_htable_value(buf, count, ppos, &data->hash_tbl->violations);
}

static int ima_measurements_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &c_ima_measurments_seqops);
}

static int ima_ascii_measurements_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &c_ima_ascii_measurements_seqops);
}

static ssize_t ima_show_measurements_count(struct file *filp,
					   char __user *buf,
					   size_t count, loff_t *ppos)
{
	unsigned int id;
	struct container_ima_data *data;
	struct task_struct *task = get_current();

	data = ima_data_exists(id);
	return ima_show_htable_value(buf, count, ppos, &data->hash_tbl->len);

}

static const struct file_operations c_ima_htable_violations_ops = {
	.read = c_ima_show_htable_violations,
	.llseek = generic_file_llseek,
};
static const struct file_operations ima_measurements_count_ops = {
	.read = ima_show_measurements_count,
	.llseek = generic_file_llseek,
};
static const struct file_operations c_ima_ascii_measurements_ops = {
	.open = ima_ascii_measurements_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};
/*
 * container_ima_fs_init
 * 		Securityfs
 *      Create a secure place to store per container measurement logs
 * 		Idea: under /integrity/ima/containers/ have a directory per container named with container id
 *      	
 */
int container_ima_fs_init(struct container_ima_data *data, struct dentry *c_ima_dir, struct dentry *c_ima_symlink) 
{
	int res;
	char *dir_name = "integrity/ima/container/";
	char *id;

	sprintf(id, "%s", &data->container_id);
	strcat(dir_name, id);

	data->container_dir = securityfs_create_dir("container_ima", c_ima_dir);
	if (IS_ERR(data->container_dir))
		return -1;

	data->binary_runtime_measurements =
	securityfs_create_file("binary_runtime_measurements",
				   S_IRUSR | S_IRGRP, data->container_dir, NULL,
				   &c_ima_measurements_ops);

	if (IS_ERR(data->binary_runtime_measurements)) {
		res = PTR_ERR(data->binary_runtime_measurements);
		goto out;
	}

	data->ascii_runtime_measurements =
	    securityfs_create_file("ascii_runtime_measurements",
				   S_IRUSR | S_IRGRP, data->container_dir, NULL,
				   &c_ima_ascii_measurements_ops);
	if (IS_ERR(data->ascii_runtime_measurements)) {
		res = PTR_ERR(data->ascii_runtime_measurements);
		goto out;
	}

	data->runtime_measurements_count =
	    securityfs_create_file("runtime_measurements_count",
				   S_IRUSR | S_IRGRP,data->container_dir, NULL,
				   &ima_measurements_count_ops);
	if (IS_ERR(data->runtime_measurements_count)) {
		res = PTR_ERR(data->runtime_measurements_count);
		goto out;
	}

	data->violations_log =
	    securityfs_create_file("violations", S_IRUSR | S_IRGRP,
				   data->container_dir, NULL, &c_ima_htable_violations_ops);
	if (IS_ERR(data->violations_log)) {
		res = PTR_ERR(data->violations_log);
		goto out;
    }
    /*
	data->c_ima_policy = securityfs_create_file("policy", POLICY_FILE_FLAGS,
					    data->container_dir, NULL,
					    &ima_measure_policy_ops);
	if (IS_ERR(data->c_ima_policy)) {
		res = PTR_ERR(data->c_ima_policy);
		goto out;
	}
    */
	return 0;
out:
	//securityfs_remove(data->c_ima_policy);
	securityfs_remove(data->violations_log);
	securityfs_remove(data->runtime_measurements_count);
	securityfs_remove(data->ascii_runtime_measurements);
	securityfs_remove(data->binary_runtime_measurements);
	securityfs_remove(data->container_dir);

	return res;
}
#endif