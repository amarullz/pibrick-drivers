#include "../hyn_core.h"
#include "cst7xx_fw.h"

#define CUSTOM_SENSOR_NUM  	(20)

#define BOOT_I2C_ADDR   (0x6A)
#define MAIN_I2C_ADDR   (0x15)
#define RW_REG_LEN   (2)

#define MODULE_ID_EN  (0)

#define CST7XX_BIN_SIZE    (15*1024)

static struct hyn_ts_data *hyn_7xxdata = NULL;

static int cst7xx_updata_judge(u8 *p_fw, u16 len);
static u32 cst7xx_read_checksum(void);
static int cst7xx_updata_tpinfo(void);
static int cst7xx_enter_boot(void);
static int cst7xx_set_workmode(enum work_mode mode,u8 enable);
static void cst7xx_rst(void);
static int write_flash_page(u16 addr,u8 *bin, u8 delay);
static int cst7xx_get_id(u32 *result);

static const struct hyn_chip_series cst7xx_fw_list[] = {
//--null--id---name--------bin
    {0,0xff,"module1",(u8*)fw_bin_1},  //default bin
    {0,0x01,"module1",(u8*)fw_bin_1},
    {0,0x02,"module2",(u8*)fw_bin_2},
    {0,0x03,"module3",(u8*)fw_bin_3},
    {0xFF,0,"null",NULL}
};

static int cst7xx_init(struct hyn_ts_data* ts_data)
{
    int ret = 0;
    u8 buf[4];
    HYN_ENTER();
    hyn_7xxdata = ts_data;
    ret = cst7xx_enter_boot();
    if(ret == FALSE){
        HYN_ERROR("cst7xx_enter_boot failed");
        return FALSE;
    }
    hyn_7xxdata->fw_updata_addr = cst7xx_fw_list[0].fw_bin; //default bin
    hyn_7xxdata->fw_updata_len = CST7XX_BIN_SIZE;

    hyn_7xxdata->hw_info.ic_fw_checksum = cst7xx_read_checksum();
    hyn_wr_reg(hyn_7xxdata,0xA006EE,3,buf,0); //exit boot
    cst7xx_rst();
    mdelay(50);
    hyn_set_i2c_addr(hyn_7xxdata,MAIN_I2C_ADDR);
    ret = cst7xx_updata_tpinfo();
    cst7xx_set_workmode(NOMAL_MODE,1);
    if(hyn_7xxdata->boot_is_pass==0 && ret != 0){ //boot fail 
       ret = cst7xx_get_id(&hyn_7xxdata->hw_info.fw_module_id);
    }
    //match fw use module_id
    HYN_INFO("MODULE_ID is %s\r\n", MODULE_ID_EN ? "enable":"disable");
#if MODULE_ID_EN
{
 	u8 i = 0;
    ret=-1;
    for(i = 0; ;i++){
        if(cst7xx_fw_list[i].moudle_id==hyn_7xxdata->hw_info.fw_module_id){
            hyn_7xxdata->fw_updata_addr = cst7xx_fw_list[i].fw_bin;
            ret = 0;
            break;
        }
    }
    HYN_INFO("module id:0x%02x match fw %s\n",hyn_7xxdata->hw_info.fw_module_id,ret ? "faild":"success");
}

#endif

    if(ret==0){
        hyn_7xxdata->need_updata_fw = cst7xx_updata_judge((u8*)hyn_7xxdata->fw_updata_addr,CST7XX_BIN_SIZE);
    }
    if(hyn_7xxdata->need_updata_fw){
        HYN_INFO("need updata FW !!!");
    }
    return TRUE;
}


static int cst7xx_get_id(u32 *result)
{
#if (HYN_POWER_ON_UPDATA && MODULE_ID_EN)
    u8 i,retry = 0,buf[4];
    u8 i2c_buf[514];
    int ret=-1;
    HYN_ENTER();
    while(++retry < 4){ 
        ret = cst7xx_enter_boot();
        if(ret){
            ret = -1;
            continue;
        } 
        memcpy(&i2c_buf[2],(u8*)fw_read_id,512);
        ret = write_flash_page(0,i2c_buf,retry);
        if(ret ==0){
            hyn_set_i2c_addr(hyn_7xxdata,MAIN_I2C_ADDR);
            i = 0;
            while(++i < 3){
                cst7xx_rst();
                msleep(20*i+50);
                ret = hyn_wr_reg(hyn_7xxdata,0xA8,1,buf,2);
                if(ret==0 && buf[1]==0xCA){ 
                    *result = buf[0];
                    return 0;
                }
            }
        }
        else{
            ret = -2;
        }
    }
    hyn_set_i2c_addr(hyn_7xxdata,MAIN_I2C_ADDR);
    HYN_INFO("read id use little bin ret = %d\r\n",ret);
    return ret;
#else
    return 0;
#endif
}

static int  cst7xx_enter_boot(void)
{
    uint8_t t;
    hyn_set_i2c_addr(hyn_7xxdata,BOOT_I2C_ADDR);
    for (t = 5;; t += 2)
    {
        int ok = FALSE;
        uint8_t i2c_buf[4] = {0};
        if (t >= 15){
            return FALSE;
        }
        cst7xx_rst();
        mdelay(t);
        ok = hyn_wr_reg(hyn_7xxdata, 0xA001AA, 3, i2c_buf, 0);
        if(ok == FALSE){
            continue;
        }
        ok = hyn_wr_reg(hyn_7xxdata, 0xA003,  2, i2c_buf, 1);
        if(ok || i2c_buf[0] != 0x55){
            continue;
        }
        break;
    }
    return TRUE;
}

static int write_flash_page(u16 addr,u8 *bin, u8 delay)
{
    int ret = 0;
    u8 i2c_buf[8],i;
    ret = hyn_wr_reg(hyn_7xxdata, U8TO32(0xA0,0x14,addr,addr>>8),4,NULL,0);
    if(ret){
        return -1;
    } 
    bin[0] = 0xA0;
    bin[1] = 0x18;
    ret = hyn_write_data(hyn_7xxdata,bin,RW_REG_LEN, 514);
    if(ret){
        return -2;
    } 
    ret = hyn_wr_reg(hyn_7xxdata, 0xA004EE,3,NULL,0);
    if(ret){
        return -3;
    } 
    mdelay(100*delay);
    for(i=50; i>0; i--){
        mdelay(5);
        ret = hyn_wr_reg(hyn_7xxdata,0xA005,2,i2c_buf,1);
        if(ret==0 && i2c_buf[0]== 0x55){
            return 0;
        }
        ret = -4;
    }
    return ret;
}


static uint32_t cst7xx_read_checksum(void)
{
    int ret = -1,time_out,retry = 3;
    uint8_t i2c_buf[4] = {0};
    uint32_t value = 0;
    hyn_7xxdata->boot_is_pass = 0;
    while(retry--){
        ret = hyn_wr_reg(hyn_7xxdata, 0xA00300,  3, i2c_buf, 0);
        if(ret) continue;
        mdelay(100);
        time_out = 100;
        while(time_out--){
            mdelay(10);
            ret = hyn_wr_reg(hyn_7xxdata, 0xA000,  2, i2c_buf, 1);
            if(0==ret && (i2c_buf[0] == 0x01 || i2c_buf[0] == 0x02)){
                hyn_7xxdata->boot_is_pass = i2c_buf[0] == 0x01 ? 1:0;
                break;
            }
            ret = -2;
        }
        if(ret) continue;
        ret = hyn_wr_reg(hyn_7xxdata, 0xA008,  2, i2c_buf, 2);
        if(ret == 0){
            value = (i2c_buf[1]<<8)|i2c_buf[0];
            break;
        }
        ret = -1;
    }
    return value;
}


static int cst7xx_updata_fw(u8 *bin_addr, u16 len)
{ 
    int retry = 0,cnt = 0,ret = -1,addr=0,offset=0;
    u8 i2c_buf[514];
    u32 fw_checksum = 0;
    // len = len;
    HYN_ENTER();
    if(len < hyn_7xxdata->fw_updata_len){
        HYN_ERROR("erro bin len \n");
        goto UPDATA_END;
    }
    len = hyn_7xxdata->fw_updata_len;
    if(0 == hyn_7xxdata->fw_file_name[0]){
        fw_checksum =U8TO16(bin_addr[5],bin_addr[4]);
    }
    else{
        ret = copy_for_updata(hyn_7xxdata,i2c_buf,4,2);
        if(ret)  goto UPDATA_END;
        fw_checksum = U8TO16(i2c_buf[1],i2c_buf[0]);
    }

    hyn_irq_set(hyn_7xxdata,DISABLE);
    hyn_esdcheck_switch(hyn_7xxdata,DISABLE);

    for(retry = 1; retry<5; retry++){
        ret = cst7xx_enter_boot();
        if(ret){
            continue;
        }
        cnt = 0;
        for(addr = 0; addr<=hyn_7xxdata->fw_updata_len-512;){
            offset = addr+6;
            if(0 == hyn_7xxdata->fw_file_name[0]){
                memcpy(&i2c_buf[2], bin_addr + offset, 512); 
            }
            else{
                ret = copy_for_updata(hyn_7xxdata,&i2c_buf[2],offset,512);
                if(ret) goto UPDATA_END;
            }
            ret = write_flash_page(addr,i2c_buf,retry);
            if(ret==0){
                addr += 512;
                cnt = 0;
                hyn_7xxdata->fw_updata_process = addr*100/len;
            }
            cnt++;
            if(cnt > 2){
                break;
            }
        }
        if(ret){
            HYN_INFO("updata failed retry:%d ret:%d\r\n",retry,ret);
            continue;
        }
        hyn_7xxdata->hw_info.ic_fw_checksum = cst7xx_read_checksum();
        if(fw_checksum != hyn_7xxdata->hw_info.ic_fw_checksum || hyn_7xxdata->boot_is_pass==0){
            hyn_7xxdata->fw_updata_process |= 0x80;
            ret = -1;
            HYN_ERROR("ic_checksum:0x%04x fw_checksum:0x%04x\r\n",hyn_7xxdata->hw_info.ic_fw_checksum,fw_checksum);
            continue;
        }
        hyn_7xxdata->fw_updata_process = 100;
        ret = 0;
        break;
    }
UPDATA_END:
    hyn_wr_reg(hyn_7xxdata,0xA006EE,3,i2c_buf,0); //exit boot
    mdelay(2);
    cst7xx_rst();
    mdelay(50);

    hyn_set_i2c_addr(hyn_7xxdata,MAIN_I2C_ADDR);   
    HYN_INFO("updata_fw %s",ret == 0 ? "success" : "failed");
    if(ret == 0){
        cst7xx_updata_tpinfo();
    }
    hyn_irq_set(hyn_7xxdata,ENABLE);
    hyn_esdcheck_switch(hyn_7xxdata,ENABLE);
    return ret;
}


static int cst7xx_updata_tpinfo(void)
{
    u8 buf[12];
    struct tp_info *ic = &hyn_7xxdata->hw_info;
    int ret = 0;

    ic->fw_module_id = 0xFF;
    ret = hyn_wr_reg(hyn_7xxdata,0xA1,1,buf,10);
    if(ret == FALSE){
        HYN_ERROR("cst7xx_updata_tpinfo failed");
        return FALSE;
    }

    ic->fw_sensor_txnum = CUSTOM_SENSOR_NUM;
    ic->fw_sensor_rxnum = 2;
    ic->fw_key_num = hyn_7xxdata->plat_data.key_num;
    ic->fw_res_y = hyn_7xxdata->plat_data.y_resolution;
    ic->fw_res_x = hyn_7xxdata->plat_data.x_resolution;

    ic->fw_project_id = U8TO32(buf[0],buf[1],buf[2],buf[3]);
    ic->fw_chip_type = buf[9];
    ic->fw_ver = buf[5];
    ic->fw_module_id = buf[7];

    HYN_INFO("IC_info fw_project_id:%x ictype:%04x fw_ver:%x checksum:%#x",ic->fw_project_id,ic->fw_chip_type,ic->fw_ver,ic->ic_fw_checksum);
    return TRUE;
}

static int cst7xx_updata_judge(u8 *p_fw, u16 len)
{
    u32 f_checksum,f_fw_ver,f_ictype,f_project_id,f_module_id;
    u8 *p_data = p_fw ; 
    u16 i,check_h =0x55;
    struct tp_info *ic = &hyn_7xxdata->hw_info;

    f_checksum = U8TO16(p_data[5],p_data[4]);
    p_data += (0x3BF4+6);
    f_project_id = U8TO32(p_data[0],p_data[1],p_data[2],p_data[3]);
    f_ictype = p_data[4];
    f_fw_ver = p_data[8];
    f_module_id = p_data[7];
    
    HYN_INFO("Bin_info fw_project_id:%x ictype:%04x fw_ver:%x checksum:%#x",f_project_id,f_ictype,f_fw_ver,f_checksum);

    p_data = p_fw+6;
    for(i=0;i<len-2;i++){
        u16 tmp;
        check_h += p_data[i];
        tmp = check_h>>15;
        check_h <<= 1;
        check_h |= tmp;
    }
    if(check_h != f_checksum){
        HYN_ERROR(".h file is damaged !! check_h:0x%04x f_checksum:0x%04x",check_h,f_checksum);
        return 0;
    }

    if(hyn_7xxdata->boot_is_pass==0  //emty
      || (ic->fw_ver <= f_fw_ver && f_checksum != ic->ic_fw_checksum)
    ){
        return 1; //need updata
    }
    return 0;
}

//------------------------------------------------------------------------------//
static int cst7xx_set_workmode(enum work_mode mode,u8 enable)
{
    int ret = -1;
    hyn_esdcheck_switch(hyn_7xxdata,mode==NOMAL_MODE? enable : DISABLE);
    switch(mode){
        case NOMAL_MODE:
            hyn_irq_set(hyn_7xxdata,ENABLE);
            ret = hyn_wr_reg(hyn_7xxdata,0xFE00,2,NULL,0);
            break;
        case GESTURE_MODE:
            ret = hyn_wr_reg(hyn_7xxdata,0xD001,2,NULL,0);
            break;
        case DIFF_MODE:
        case RAWDATA_MODE:
            ret = hyn_wr_reg(hyn_7xxdata,mode==DIFF_MODE ? 0xBF07:0xBF06,2,NULL,0);
            break;
        case FAC_TEST_MODE:
            hyn_write_data(hyn_7xxdata,(u8[]){0xb6,0x40,0x40,0x20},1,4);
            hyn_wr_reg(hyn_7xxdata,0xF001,2,NULL,0);
            msleep(50);
            break;
        case DEEPSLEEP:
            hyn_irq_set(hyn_7xxdata,DISABLE);
            ret = hyn_wr_reg(hyn_7xxdata,0xA503,2,NULL,0);
            break;
        case ENTER_BOOT_MODE:
            ret |= cst7xx_enter_boot();
            break;
        case CHARGE_EXIT:
        case CHARGE_ENTER:
            hyn_7xxdata->charge_is_enable = mode&0x01;
            ret = hyn_wr_reg(hyn_7xxdata,(mode&0x01)? 0xE601:0xE600,2,0,0); //charg mode
            mode = hyn_7xxdata->work_mode; //not switch work mode
            break;
        default :
            ret = -2;
            break;
    }
    if(ret != -2){
        hyn_7xxdata->work_mode = mode;
    }
    return ret;
}

static int cst7xx_prox_handle(u8 cmd)
{
    int ret = 0;
    switch(cmd){
        case 1: //enable
            hyn_7xxdata->prox_is_enable = 1;
            hyn_7xxdata->prox_state = 0;
            ret = hyn_wr_reg(hyn_7xxdata,0xB001,2,NULL,0);
            break;
        case 0: //disable
            hyn_7xxdata->prox_is_enable = 0;
            hyn_7xxdata->prox_state = 0;
            ret = hyn_wr_reg(hyn_7xxdata,0xB000,2,NULL,0);
            break;
        default: 
            break;
    }
    return ret;
}

static void cst7xx_rst(void)
{
    if(hyn_7xxdata->work_mode==ENTER_BOOT_MODE){
        hyn_set_i2c_addr(hyn_7xxdata,MAIN_I2C_ADDR);
    }
    gpio_set_value(hyn_7xxdata->plat_data.reset_gpio,0);
    msleep(10);
    gpio_set_value(hyn_7xxdata->plat_data.reset_gpio,1);
}

static int cst7xx_supend(void)
{
    HYN_ENTER();
    cst7xx_set_workmode(DEEPSLEEP,0);
    return 0;
}

static int cst7xx_resum(void)
{
    cst7xx_rst();
    msleep(50);
    cst7xx_set_workmode(NOMAL_MODE,1);
    return 0;
}

static int cst7xx_report(void)
{
    uint8_t i = 0;
    uint8_t i2c_buf[3+6*MAX_POINTS_REPORT] = {0};
    uint8_t id = 0,index = 0;
    struct hyn_plat_data *dt = &hyn_7xxdata->plat_data;

    memset(&hyn_7xxdata->rp_buf,0,sizeof(hyn_7xxdata->rp_buf));
    hyn_7xxdata->rp_buf.report_need = REPORT_NONE;

    
    if(hyn_7xxdata->work_mode == GESTURE_MODE){
        static const uint8_t ges_map[][2] = {{0x24,IDX_POWER},{0x22,IDX_UP},{0x23,IDX_DOWN},{0x20,IDX_LEFT},{0x21,IDX_RIGHT},
        {0x34,IDX_C},{0x33,IDX_e},{0x32,IDX_M},{0x30,IDX_O},{0x46,IDX_S},{0x54,IDX_V},{0x31,IDX_W},{0x65,IDX_Z}};
        if(hyn_wr_reg(hyn_7xxdata,0xD3,1,i2c_buf,1)){
            goto FAILD_END;
        }
        index = sizeof(ges_map)/2;
        hyn_7xxdata->gesture_id  = IDX_NULL;
        for(i=0; i<index; i++){
            if(ges_map[i][0] == i2c_buf[0]){
                hyn_7xxdata->gesture_id = ges_map[i][1];
                hyn_7xxdata->rp_buf.report_need = REPORT_GES;
                break;
            }
        }
        return TRUE;
    }
    else{
        int ret = -1,retry = 3;
        u8 event = 0;
        while(--retry){
            ret = hyn_wr_reg(hyn_7xxdata,0x00,1,i2c_buf,(3+6*2));
            if(ret == 0 && i2c_buf[2] < 3) break;
			ret = -1;
        }
        if(ret){
            goto FAILD_END;
        }
        if(hyn_7xxdata->prox_is_enable){
            u8 state=0;
            if(i2c_buf[1]==0xE0 || i2c_buf[1]==0){
                state = PS_FAR_AWAY;
            }
            else if(i2c_buf[1]==0xC0){
                state = PS_NEAR;
            }
            if(hyn_7xxdata->prox_state != state){
				hyn_7xxdata->prox_state = state;
                hyn_7xxdata->rp_buf.report_need |= REPORT_PROX;
            }
        }
        hyn_7xxdata->rp_buf.rep_num  = i2c_buf[2]&0x0F;
        for(i = 0 ; i < 2 ; i++){
            id = (i2c_buf[5 + i*6] & 0xf0)>>4;
            event = i2c_buf[3 + i*6]&0xC0;
            if(id > 1 || event==0xC0) continue;
            hyn_7xxdata->rp_buf.pos_info[index].pos_id = id;
            hyn_7xxdata->rp_buf.pos_info[index].event = 0x40 == event ? 0:1;
            hyn_7xxdata->rp_buf.pos_info[index].pos_x = ((u16)(i2c_buf[3 + i*6] & 0x0f)<<8) + i2c_buf[4 + i*6];
            hyn_7xxdata->rp_buf.pos_info[index].pos_y = ((u16)(i2c_buf[5 + i*6] & 0x0f)<<8) + i2c_buf[6 + i*6];
            // hyn_7xxdata->rp_buf.pos_info[index].pres_z = (i2c_buf[7 + i*6] <<8) + i2c_buf[8 + i*6] ;
            hyn_7xxdata->rp_buf.pos_info[index].pres_z = 3+(hyn_7xxdata->rp_buf.pos_info[index].pos_x&0x03); //press mast chang
            index++;
        }
        if(index){
            hyn_7xxdata->rp_buf.report_need |= REPORT_POS;
        }

        if(dt->key_num){
            i = dt->key_num;
            while(i){
                i--;
                if(dt->key_y_coords ==hyn_7xxdata->rp_buf.pos_info[0].pos_y && dt->key_x_coords[i] == hyn_7xxdata->rp_buf.pos_info[0].pos_x){
                    hyn_7xxdata->rp_buf.key_id = i;
                    hyn_7xxdata->rp_buf.key_state = hyn_7xxdata->rp_buf.pos_info[0].event;
                    hyn_7xxdata->rp_buf.report_need = REPORT_KEY;
                }
            }
        }
    }
    return TRUE;
    FAILD_END:
    HYN_ERROR("read report data failed");
    return FALSE;
}

static u32 cst7xx_check_esd(void)
{
    int ret = -1;
    u8 buf[4];
    ret = hyn_wr_reg(hyn_7xxdata,0xAE,1,buf,2);
    return ret ? -1:U8TO16(buf[1],buf[0]);
}


static int cst7xx_get_dbg_data(u8 *buf, u16 len)
{
    int ret = -1;
    switch (hyn_7xxdata->work_mode){
        case DIFF_MODE:
        case RAWDATA_MODE:
            ret = hyn_wr_reg(hyn_7xxdata, 0x2B, 1,buf,len); 
            if(ret==0){
                exchange_byte(buf,len);
            }
            break;
        default:
            HYN_ERROR("work_mode:%d",hyn_7xxdata->work_mode);
            break;
    }
    return len;
}


static int cst7xx_get_test_result(u8 *buf, u16 len)
{
    int ret = -1;
    u8 time_out = 200;
    if(len > CUSTOM_SENSOR_NUM*2){
        len = CUSTOM_SENSOR_NUM*2;
    }
    while(--time_out){
        msleep(10);
        ret = hyn_wr_reg(hyn_7xxdata, 0xF0, 1,buf,1); 
        if(ret == 0 && buf[0]==0){
            break;
        }
        ret = FAC_GET_DATA_FAIL;
    }
    if(ret==0){
        ret = hyn_wr_reg(hyn_7xxdata, 0x2B, 1,buf,len); 
        if(ret==0){
            exchange_byte(buf,len);
        }
    }
    return ret;
}


const struct hyn_ts_fuc cst7xx_fuc = {
    .tp_rest = cst7xx_rst,
    .tp_report = cst7xx_report,
    .tp_supend = cst7xx_supend,
    .tp_resum = cst7xx_resum,
    .tp_chip_init = cst7xx_init,
    .tp_updata_fw = cst7xx_updata_fw,
    .tp_set_workmode = cst7xx_set_workmode,
    .tp_check_esd = cst7xx_check_esd,
    .tp_prox_handle = cst7xx_prox_handle,
    .tp_get_dbg_data = cst7xx_get_dbg_data,
    .tp_get_test_result = cst7xx_get_test_result
};


