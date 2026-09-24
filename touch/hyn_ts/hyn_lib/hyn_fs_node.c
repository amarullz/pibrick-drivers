
#include "../hyn_core.h"
#include <linux/string.h>
#include <linux/kernel.h>


static struct hyn_ts_data *hyn_fs_data = NULL;

//echo fd /sdcard/app.bin
//echo rst
#define DEBUG_BUF_SIZE (128)
//host_cmd_save
#define READ_IIC 	    (0x3A)
#define NULL_DATA       (0x00)

static  ssize_t hyn_dbg_show(struct device *dev,struct device_attribute *attr,char *buf)
{
	ssize_t len = 0;
    int ret = -1;
	u8 *kbuf =NULL;
	u16 i;
	HYN_ENTER();
	mutex_lock(&hyn_fs_data->mutex_fs);

	if(hyn_fs_data->host_cmd_save[0] == READ_IIC){
		u8 r_len = hyn_fs_data->host_cmd_save[1];
		kbuf = kzalloc(DEBUG_BUF_SIZE, GFP_KERNEL);
		if(IS_ERR_OR_NULL(kbuf)){
			goto dbg_sh_end;
			HYN_ERROR("kzalloc failed \r\n");
		}
		ret = hyn_read_data(hyn_fs_data,kbuf,r_len); 
		if(ret ==0){
			for(i = 0; i< r_len; i++){
				len += sprintf(buf+len,"0x%02x ",*(kbuf+i));
			}
			buf[len] = '\n';
			len++;
		}
		kfree(kbuf);
	}

dbg_sh_end:
	mutex_unlock(&hyn_fs_data->mutex_fs);
	return len;
}


static  ssize_t hyn_dbg_store(struct device *dev,struct device_attribute *attr,const char *buf, size_t count)
{
	u8 str[64]={'\0'};
	u8 *next_ptr = (u8*)buf;
	int tmp;
	const struct hyn_ts_fuc* hyn_fun = hyn_fs_data->hyn_fuc_used;
	int ret = 0;
	HYN_ENTER();
	mutex_lock(&hyn_fs_data->mutex_fs);
	
	ret = get_word(&next_ptr,str);
	HYN_INFO("word:%s %d\n",str,ret);
	if(0 == strcmp(str,"rst")){
		hyn_fun->tp_rest();
	}
	else if(0 == strcmp(str,"w") || 0 == strcmp(str,"r")){
		u8 *w_buf = kzalloc(count/2, GFP_KERNEL);
		u8 w_len=0, wr_flg=str[0];
		if(wr_flg=='w'){
			while(*(next_ptr-1) != 0x0A && *(next_ptr-1) != '\0'){
				if(get_word(&next_ptr,str)==0) break;
				if(0 == strcmp(str,"r")){
					wr_flg='r';
					break;
				}
				if(str_2_num(str,&tmp,16)) break;
				w_buf[w_len++] = tmp;
			}
		}
		if(w_len){
			hyn_write_data(hyn_fs_data,w_buf,2,w_len);
		}
		if(wr_flg=='r'){
			hyn_fs_data->host_cmd_save[0] = READ_IIC;
			hyn_fs_data->host_cmd_save[1] = 1;
			if(get_word(&next_ptr,str)){
				if(0== str_2_num(str,&tmp,16)){
					hyn_fs_data->host_cmd_save[1] = tmp;
				}
			}
		}
		kfree(w_buf);
	}
	else if(0 == strcmp(str,"log")){
		ret = get_word(&next_ptr,str);
		hyn_fs_data->log_level = str[0]-'0';
	}
	else if(0 == strcmp(str,"mode")){
		ret = get_word(&next_ptr,str);
		if(ret){
			char mode = str[0]-'0';
			ret = get_word(&next_ptr,str);
			if(ret) hyn_fun->tp_set_workmode(mode,str[0]=='0'? 0:1);
		}
	}
	else if(0 == strcmp(str,"fd")){
		ret = get_word(&next_ptr,str);
		if(strlen(str)<4){
			strcpy(str,"/sdcard/app.bin");
		}
		HYN_INFO("filename = %s",str);
		strcpy(hyn_fs_data->fw_file_name,str);
		ret = hyn_fun->tp_updata_fw(hyn_fs_data->fw_updata_addr,hyn_fs_data->fw_updata_len);
	}

	mutex_unlock(&hyn_fs_data->mutex_fs);
	return count;
}

///cat hyntpfwver
static  ssize_t hyn_tpfwver_show(struct device *dev,	struct device_attribute *attr,char *buf)
{
	ssize_t num_read = 0;
    HYN_ENTER();
	num_read = snprintf(buf, 128, "fw_version:0x%02X,chip_type:0x%02X,checksum:0x%02X,project_id:%04x\n",
			   hyn_fs_data->hw_info.fw_ver,
			   hyn_fs_data->hw_info.fw_chip_type,
			   hyn_fs_data->hw_info.ic_fw_checksum,
			   hyn_fs_data->hw_info.fw_project_id
			   );
	return num_read;
}

static  ssize_t hyn_tpfwver_store(struct device *dev,struct device_attribute *attr,const char *buf, size_t count)
{
	/*place holder for future use*/
	return -EPERM;
}


///cat hyntpfwver
static  ssize_t hyn_mode_show(struct device *dev,	struct device_attribute *attr,char *buf)
{
	ssize_t num_read = 0;
    HYN_ENTER();
	num_read = snprintf(buf, 128, "is_charge:%d, is_glove:%d",hyn_fs_data->charge_is_enable,hyn_fs_data->glove_is_enable);
	return num_read;
}

static  ssize_t hyn_mode_store(struct device *dev,struct device_attribute *attr,const char *buf, size_t count)
{
	const struct hyn_ts_fuc* hyn_fun = hyn_fs_data->hyn_fuc_used;
	HYN_ENTER();
	HYN_INFO("wlen:%d,%c%c",(int)count,buf[0],buf[1]);
	if(buf[0]=='c' || buf[0]=='C'){
		hyn_fun->tp_set_workmode(buf[1]=='1' ? CHARGE_ENTER:CHARGE_EXIT,1);
	}
	else if(buf[0]=='g' || buf[0]=='G'){
		hyn_fun->tp_set_workmode(buf[1]=='1' ? GLOVE_ENTER:GLOVE_EXIT,1);
	}
	return count;
}

//hyndumpfw
static  ssize_t hyn_dumpfw_show(struct device *dev,struct device_attribute *attr,char *buf)
{
    HYN_ENTER();
	hyn_dump_fw(hyn_fs_data,NULL,0);
	return (ssize_t)snprintf(buf, 128, "place reserver");
}

static ssize_t hyn_dumpfw_store(struct device *dev,struct device_attribute *attr,const char *buf, size_t count)
{
	if((int)count<16){
		u8 *next_ptr = (u8*)buf;
		u8 str[16]={'\0'};
		get_word(&next_ptr,str);
		if(0 == strcmp(str,"fwstart")){
			hyn_fs_data->fw_dump_state = 1;
			hyn_dump_fw(hyn_fs_data,(u8*)buf,count);
		}
		else if(0 == strcmp(str,"fwend")){
			hyn_dump_fw(hyn_fs_data,NULL,0);
		}
		else{
			hyn_dump_fw(hyn_fs_data,(u8*)buf,count);
		}
	}
	else{
		hyn_dump_fw(hyn_fs_data,(u8*)buf,count);
	}

	return count;
}


static DEVICE_ATTR(hyntpfwver, S_IRUGO | S_IWUSR, hyn_tpfwver_show, hyn_tpfwver_store);
static DEVICE_ATTR(hyntpdbg, S_IRUGO | S_IWUSR, hyn_dbg_show, hyn_dbg_store);
static DEVICE_ATTR(hyndumpfw, S_IRUGO | S_IWUSR, hyn_dumpfw_show, hyn_dumpfw_store);
static DEVICE_ATTR(hynswitchmode, S_IRUGO | S_IWUSR, hyn_mode_show, hyn_mode_store);

static struct attribute *hyn_attributes[] = {
	&dev_attr_hynswitchmode.attr,
	&dev_attr_hyndumpfw.attr,
	&dev_attr_hyntpdbg.attr,
	&dev_attr_hyntpfwver.attr,
	NULL
};

static struct attribute_group hyn_attribute_group = {
	.attrs = hyn_attributes
};

int hyn_create_sysfs(struct hyn_ts_data *ts_data)
{
	int ret = 0;
    hyn_fs_data = ts_data;
	hyn_fs_data->fw_dump_state = 0;
    ts_data->sys_node = &ts_data->dev->kobj;//kobject_create_and_add("hynitron_debug", NULL);
	if(IS_ERR_OR_NULL(ts_data->sys_node)){
		HYN_ERROR("kobject_create_and_add failed");
		return -1;
	}
    ret = sysfs_create_group(ts_data->sys_node, &hyn_attribute_group);
	// if(ret){
	// 	kobject_put(ts_data->sys_node);
	// 	return -1;
	// }
	HYN_INFO("sys_node creat success ,GKI version is [%s]",HYN_GKI_VER ? "enable":"disable");
	return ret;
}

void hyn_release_sysfs(struct hyn_ts_data *ts_data)
{
    if(!IS_ERR_OR_NULL(ts_data->sys_node)){
		sysfs_remove_group(ts_data->sys_node, &hyn_attribute_group);
		//kobject_put(ts_data->sys_node);
		ts_data->sys_node = NULL;
	}
}

