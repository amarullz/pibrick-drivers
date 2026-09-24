#include "../hyn_core.h"
#include "cst92xx_fw.h"

#define BOOT_I2C_ADDR   (0x5A)
#define MAIN_I2C_ADDR   (0x5A)
#define RW_REG_LEN   (2)

#define MAX_FINGER (2)

#define CST92XX_BIN_SIZE    (0x7F80)

#define HYNITRON_PROGRAM_PAGE_SIZE (128)

static struct hyn_ts_data *hyn_92xxdata = NULL;

static int cst92xx_updata_judge(u8 *p_fw, u16 len);
static u32 cst92xx_read_checksum(void);
static int cst92xx_updata_tpinfo(void);
static int cst92xx_enter_boot(void);
static void cst92xx_rst(void);
static int cst92xx_set_workmode(enum work_mode mode,u8 enable);

static int cst92xx_init(struct hyn_ts_data* ts_data)
{
    int ret = 0;
    HYN_ENTER();
    hyn_92xxdata = ts_data;
    hyn_set_i2c_addr(hyn_92xxdata,MAIN_I2C_ADDR);
    cst92xx_rst();
    msleep(40);
    ret = cst92xx_updata_tpinfo();
    if(ret == FALSE){
        HYN_INFO("cst92xx_updata_tpinfo failed");
        hyn_92xxdata->work_mode=ENTER_BOOT_MODE;
        ret = cst92xx_enter_boot();
        if(ret){
            HYN_ERROR("cst92xx_ic check failed\r\n");
            return FALSE;
        }
        cst92xx_rst();
    }
    hyn_92xxdata->fw_updata_addr = (u8*)fw_bin;
    hyn_92xxdata->fw_updata_len = CST92XX_BIN_SIZE;
    hyn_92xxdata->need_updata_fw = cst92xx_updata_judge((u8*)fw_bin,CST92XX_BIN_SIZE);
    HYN_INFO("cst92xx_init done !!!");
    return TRUE;
}


static int  cst92xx_enter_boot(void)
{
    int ok = FALSE,t,retry = 3;
    uint8_t i2c_buf[4] = {0};
    hyn_set_i2c_addr(hyn_92xxdata,BOOT_I2C_ADDR);
    for (t = 10;; t += 2)
    {
        if(t >= 30 || retry==0){
            return FALSE;
        }

        cst92xx_rst();
        mdelay(t);

        ok = hyn_wr_reg(hyn_92xxdata, 0xA001AA, 3, i2c_buf, 0);
        if(ok == FALSE || (i2c_buf[0]==0x5A && i2c_buf[1]==0x5A)){
            if(ok==TRUE && retry){
                retry--;
                t = 10;
            }
            continue;
        }
        mdelay(1);
        ok = hyn_wr_reg(hyn_92xxdata, 0xA002,  2, i2c_buf, 2);
        if(ok == FALSE){
            continue;
        }
        if ((i2c_buf[0] == 0x55) && (i2c_buf[1] == 0xB0)) {
            break;
        }
    }
    ok = hyn_wr_reg(hyn_92xxdata, 0xA00100, 3, i2c_buf, 0);
    if(ok == FALSE){
        return FALSE;
    }
    return TRUE;
}

static int erase_all_mem(void)
{
    int ok = FALSE,t;
    u8 i2c_buf[8];

	//erase_all_mem
    ok = hyn_wr_reg(hyn_92xxdata, 0xA0140000, 4, i2c_buf, 0);
    if (ok == FALSE){
        return FALSE;
    }
    ok = hyn_wr_reg(hyn_92xxdata, 0xA00C807F, 4, i2c_buf, 0);
    if (ok == FALSE){
        return FALSE;
    }
    ok = hyn_wr_reg(hyn_92xxdata, 0xA004EC, 3, i2c_buf, 0);
    if (ok == FALSE){
        return FALSE;
    }
        
    mdelay(300);
    for (t = 0;; t += 10) {
        if (t >= 1000) {
           return FALSE;
        }

        mdelay(10);

        ok = hyn_wr_reg(hyn_92xxdata, 0xA005, 2, i2c_buf, 1);
        if (ok == FALSE) {
            continue;
        }

        if (i2c_buf[0] == 0x88) {
            break;
        }
    }

    return TRUE;
}


static int write_mem_page(uint16_t addr, uint8_t *buf, uint16_t len)
{
    int ok = FALSE,t;
    uint8_t i2c_buf[1024+2] = {0};

    i2c_buf[0] = 0xA0;
    i2c_buf[1] = 0x0C;
    i2c_buf[2] = len;
    i2c_buf[3] = len >> 8;
    //ok = hyn_i2c_write_r16(HYN_BOOT_I2C_ADDR, 0xA00C, i2c_buf, 2);
    ok = hyn_write_data(hyn_92xxdata, i2c_buf,RW_REG_LEN, 4);
    if(ok == FALSE){
         return FALSE;
    }


    i2c_buf[0] = 0xA0;
    i2c_buf[1] = 0x14;
    i2c_buf[2] = addr;
    i2c_buf[3] = addr >> 8;
    ok = hyn_write_data(hyn_92xxdata, i2c_buf,RW_REG_LEN, 4);
    if(ok == FALSE) {
        return FALSE;
    }


    i2c_buf[0] = 0xA0;
    i2c_buf[1] = 0x18;
    memcpy(i2c_buf + 2, buf, len);            
    ok = hyn_write_data(hyn_92xxdata, i2c_buf,RW_REG_LEN, len+2);
    if(ok == FALSE){
        return FALSE;
    }


    ok =  hyn_wr_reg(hyn_92xxdata,0xA004EE,3,i2c_buf,0);
    if(ok == FALSE){
        return FALSE;
    }

    for (t = 0;; t += 10) {
        if (t >= 1000) {
            return FALSE;
        }

        mdelay(5);

        ok =  hyn_wr_reg(hyn_92xxdata,0xA005,2,i2c_buf,1);
        if(ok == FALSE){
            continue;
        }        

        if (i2c_buf[0] == 0x55) {
            break;
        }
    }

    return TRUE;
}


static int write_code(u8 *bin_addr,uint8_t retry)
{
    uint8_t data[HYNITRON_PROGRAM_PAGE_SIZE+4];//= (uint8_t *)bin_addr;
    uint16_t addr = 0;
    uint16_t remain_len = CST92XX_BIN_SIZE;
    int ret;
   
    while (remain_len > 0) {
        uint16_t cur_len = remain_len;
        if (cur_len > HYNITRON_PROGRAM_PAGE_SIZE) {
            cur_len = HYNITRON_PROGRAM_PAGE_SIZE;
        }
        
        if(0 == hyn_92xxdata->fw_file_name[0]){
            memcpy(data, bin_addr + addr, HYNITRON_PROGRAM_PAGE_SIZE);
        }else{
            ret = copy_for_updata(hyn_92xxdata,data,addr,HYNITRON_PROGRAM_PAGE_SIZE);
            if(ret == FALSE){
                HYN_ERROR("copy_for_updata error");
                return FALSE;   
            }
        }
        //HYN_INFO("write_code addr 0x%x 0x%x",addr,*data);
        if (write_mem_page(addr, data, cur_len) ==  FALSE) {
             return FALSE;
        }
        //data += cur_len;
        addr += cur_len;
        remain_len -= cur_len;
    }
    return TRUE;
}


static uint32_t cst92xx_read_checksum(void)
{
    int ok = FALSE;
    uint8_t i2c_buf[4] = {0};
    uint32_t chip_checksum = 0;
    uint8_t retry = 5;
    
    ok =  hyn_wr_reg(hyn_92xxdata,0xA00300,3,i2c_buf,0);
    if (ok == FALSE) {
        return FALSE;
    }      
    mdelay(2);    
    while(retry--){
        mdelay(5);
        ok =  hyn_wr_reg(hyn_92xxdata,0xA000,2,i2c_buf,1);
        if (ok == FALSE) {
            continue;
        }
        if(i2c_buf[0]!=0) break;
    }

    mdelay(1);
     if(i2c_buf[0] == 0x01){
        memset(i2c_buf,0,sizeof(i2c_buf));
        ok =  hyn_wr_reg(hyn_92xxdata,0xA008,2,i2c_buf,4);
        if (ok == FALSE) {
            return FALSE;
        }      
        chip_checksum = U8TO32(i2c_buf[3],i2c_buf[2],i2c_buf[1],i2c_buf[0]);
    }
    else{
        hyn_92xxdata->need_updata_fw = 1;
    }

    return chip_checksum;
}


static int cst92xx_updata_fw(u8 *bin_addr, u16 len)
{ 
    #define CHECKSUM_OFFECT  (0x7F6C)
    int retry = 0;
    int ok_copy = TRUE;
    int ok = FALSE;
    u8 i2c_buf[4];

    u32 fw_checksum=0;
    HYN_ENTER();

    if(len < CST92XX_BIN_SIZE){
        HYN_ERROR("len = %d",len);
        goto UPDATA_END;
    }
    if(len > CST92XX_BIN_SIZE) len = CST92XX_BIN_SIZE;

    if(0 != hyn_92xxdata->fw_file_name[0]){
        //node to update
        ok = copy_for_updata(hyn_92xxdata,i2c_buf,CST92XX_BIN_SIZE-20,4);
        fw_checksum = U8TO32(i2c_buf[3],i2c_buf[2],i2c_buf[1],i2c_buf[0]);
        if(hyn_92xxdata->hw_info.ic_fw_checksum == fw_checksum || ok != 0){
             HYN_INFO("no update,fw_checksum is same:0x%04x",fw_checksum);
             goto UPDATA_END;
        }
    }else{
        fw_checksum = U8TO32(bin_addr[CHECKSUM_OFFECT+3],bin_addr[CHECKSUM_OFFECT+2],bin_addr[CHECKSUM_OFFECT+1],bin_addr[CHECKSUM_OFFECT+0]);
    }
    HYN_INFO("updating fw checksum:0x%04x",fw_checksum);

    hyn_irq_set(hyn_92xxdata,DISABLE);
    hyn_esdcheck_switch(hyn_92xxdata,DISABLE);
    hyn_set_i2c_addr(hyn_92xxdata,BOOT_I2C_ADDR);
    
    HYN_INFO("updata_fw start");
    for(retry = 1; retry<5; retry++){
        hyn_92xxdata->fw_updata_process = 0;
        ok = cst92xx_enter_boot();
        if (ok == FALSE){
            continue;
        }
        hyn_92xxdata->fw_updata_process = 10;
        ok = erase_all_mem();
        if (ok == FALSE){
            continue;
        }
        hyn_92xxdata->fw_updata_process = 20;
        ok = write_code(bin_addr,retry);
        if (ok == FALSE){
            continue;
        }
        hyn_92xxdata->fw_updata_process = 30;
        hyn_92xxdata->hw_info.ic_fw_checksum = cst92xx_read_checksum();
        if(fw_checksum != hyn_92xxdata->hw_info.ic_fw_checksum){
            HYN_INFO("out data fw checksum err:0x%04x",hyn_92xxdata->hw_info.ic_fw_checksum);
            hyn_92xxdata->fw_updata_process |= 0x80;
            continue;
        }
        hyn_92xxdata->fw_updata_process = 100;   
        if(retry>=5){
            ok_copy = FALSE;
            break;
        }
        break;
    }

    hyn_wr_reg(hyn_92xxdata,0xA006EE,3,i2c_buf,0); //exit boot
    mdelay(2);

UPDATA_END:   
    cst92xx_rst();
    mdelay(50);

    hyn_set_i2c_addr(hyn_92xxdata,MAIN_I2C_ADDR);   

    if(ok_copy == TRUE){
        cst92xx_updata_tpinfo();
        HYN_INFO("updata_fw success");
    }
    else{
        HYN_INFO("updata_fw failed");
    }

    hyn_irq_set(hyn_92xxdata,ENABLE);

    return ok_copy;
}

// static int16_t read_word_from_mem(uint8_t type, uint16_t addr, uint32_t *value)
// {
//     int16_t ret = 0;
//     uint8_t i2c_buf[4] = {0},t;

//     i2c_buf[0] = 0xA0;
//     i2c_buf[1] = 0x10;
//     i2c_buf[2] = type;
//     ret = hyn_write_data(hyn_92xxdata,i2c_buf,2,3); 
//     if (ret)
//     {
//         return -1;
//     }

//     i2c_buf[0] = 0xA0;
//     i2c_buf[1] = 0x0C;
//     i2c_buf[2] = addr;
//     i2c_buf[3] = addr >> 8;
//     ret = hyn_write_data(hyn_92xxdata,i2c_buf,2,4); 
//     if (ret)
//     {
//         return -1;
//     }

//     i2c_buf[0] = 0xA0;
//     i2c_buf[1] = 0x04;
//     i2c_buf[2] = 0xE4;
//     ret = hyn_write_data(hyn_92xxdata,i2c_buf,2,3); 
//     if (ret)
//     {
//         return -1;
//     }

//     for (t = 0;; t++)
//     {
//         if (t >= 100)
//         {
//             return -1;
//         }
//         ret =hyn_wr_reg(hyn_92xxdata,0xA004,2,i2c_buf,1);
//         if (ret)
//         {
//             continue;
//         }
//         if (i2c_buf[0] == 0x00)
//         {
//             break;
//         }
//     }
//     ret =hyn_wr_reg(hyn_92xxdata,0xA018,2,i2c_buf,4);
//     if (ret)
//     {
//         return -1;
//     }
//     *value = ((uint32_t)(i2c_buf[0])) |
//              (((uint32_t)(i2c_buf[1])) << 8) |
//              (((uint32_t)(i2c_buf[2])) << 16) |
//              (((uint32_t)(i2c_buf[3])) << 24);

//     return 0;
// }

// static int cst92xx_read_chip_id(void)
// {
//     int16_t ret = 0;
//     uint8_t retry = 3;
//     uint32_t partno_chip_type,module_id;

//     ret = cst92xx_enter_boot();
//     if (ret == FALSE)
//     {
//         HYN_ERROR("enter_bootloader error");
//         return -1;
//     }
//     for (; retry > 0; retry--)
//     {
//         // partno
//         ret = read_word_from_mem(1, 0x077C, &partno_chip_type);
//         if (ret)
//         {
//             continue;
//         }
//         // module id
//         ret = read_word_from_mem(0, 0x7FC0, &module_id);
//         if (ret)
//         {
//             continue;
//         }
//         if ((partno_chip_type >> 16) == 0xCACA)
//         {
//             partno_chip_type &= 0xffff;
//             break;
//         }
//     }
//     cst92xx_rst();
//     msleep(30);
//     HYN_INFO("partno_chip_type: 0x%04x", partno_chip_type);
//     HYN_INFO("module_id: 0x%04x", module_id);
//     if ((partno_chip_type != 0x9217) && (partno_chip_type != 0x9220))
//     {
//         HYN_ERROR("partno_chip_type error 0x%04x", partno_chip_type);
//         //return -1;
//     }
//     return 0;
// }

static int cst92xx_updata_tpinfo(void)
{
    u8 buf[30],retry=8;
    struct tp_info *ic = &hyn_92xxdata->hw_info;
    int ret = 0;
    hyn_92xxdata->boot_is_pass = 0;
    while(retry--){
        cst92xx_set_workmode(0xff,DISABLE);
        ret = hyn_wr_reg(hyn_92xxdata,0xD101,2,buf,0);
        if(ret) continue;
        ret = hyn_wr_reg(hyn_92xxdata,0xD1F4,2,buf,28);
        if(ret==0 && (buf[19]==0x92||buf[19]==0x91)){
            break;
        }
        ret = -1;
    }
    if(ret){
         return FALSE;
    }
    hyn_92xxdata->boot_is_pass = 1;
    ic->fw_project_id = ((uint16_t)buf[17] <<8) + buf[16];
    ic->fw_chip_type = ((uint16_t)buf[19] <<8) + buf[18];

    //firmware_version
    ic->fw_ver = (buf[23]<<24)|(buf[22]<<16)|(buf[21]<<8)|buf[20];

    //tx_num   rx_num   key_num
    ic->fw_sensor_txnum = ((uint16_t)buf[1]<<8) + buf[0];
    ic->fw_sensor_rxnum = buf[2];
    ic->fw_key_num = buf[3];

    ic->fw_res_y = (buf[7]<<8)|buf[6];
    ic->fw_res_x = (buf[5]<<8)|buf[4];

    //fw_checksum
    ic->ic_fw_checksum = (buf[27]<<24)|(buf[26]<<16)|(buf[25]<<8)|buf[24];

    HYN_INFO("IC_info project_id:%04x ictype:%04x fw_ver:%x checksum:%#x",ic->fw_project_id,ic->fw_chip_type,ic->fw_ver,ic->ic_fw_checksum);
   
    cst92xx_set_workmode(NOMAL_MODE,ENABLE);
   
    return TRUE;
}

static int cst92xx_updata_judge(u8 *p_fw, u16 len)
{
    u32 f_checksum,f_fw_ver,f_ictype,f_fw_project_id;
    u8 *p_data = p_fw + len - 28;   //7F64
    struct tp_info *ic = &hyn_92xxdata->hw_info;

    f_fw_project_id = U8TO16(p_data[1],p_data[0]);
    f_ictype = U8TO16(p_data[3],p_data[2]);

    f_fw_ver = U8TO16(p_data[7],p_data[6]);
    f_fw_ver = (f_fw_ver<<16)|U8TO16(p_data[5],p_data[4]);

    f_checksum = U8TO16(p_data[11],p_data[10]);
    f_checksum = (f_checksum << 16)|U8TO16(p_data[9],p_data[8]);


    HYN_INFO("Bin_info project_id:0x%04x ictype:0x%04x fw_ver:0x%x checksum:0x%x",f_fw_project_id,f_ictype,f_fw_ver,f_checksum);
    if(
        (f_checksum != ic->ic_fw_checksum && f_fw_ver >= ic->fw_ver && f_ictype == ic->fw_chip_type)
        || (hyn_92xxdata->boot_is_pass==0) //empty chip
    ){
        HYN_INFO("need update!");
        return 1; //need updata
    }
    HYN_INFO("cst92xx_updata_judge done, no need update");
    return 0;
}

//------------------------------------------------------------------------------//

static int cst92xx_set_workmode(enum work_mode mode,u8 enable)
{
    int ok = FALSE;
    uint8_t i2c_buf[4] = {0};
    uint8_t i = 0;
    hyn_92xxdata->work_mode = mode;

    for(i=0;i<3;i++){
        ok = hyn_wr_reg(hyn_92xxdata,0xD11E,2,NULL,0);
        udelay(200);
        ok |= hyn_wr_reg(hyn_92xxdata,0x0002,2,i2c_buf,2);
        if(ok==TRUE && i2c_buf[1] == 0x1E){
            break;
        }     
    }    
    hyn_esdcheck_switch(hyn_92xxdata,enable);
    switch(mode){
        case NOMAL_MODE:
            hyn_irq_set(hyn_92xxdata,ENABLE);
            ok = hyn_wr_reg(hyn_92xxdata,0xD109,2,NULL,0);
            break;
        case GESTURE_MODE:
            ok = hyn_wr_reg(hyn_92xxdata,0xD104,2,NULL,0);
            break;
        case LP_MODE:
            ok = hyn_wr_reg(hyn_92xxdata,0xD107,2,NULL,0);
            break;
        case DIFF_MODE:
            ok = hyn_wr_reg(hyn_92xxdata,0xD10D,2,NULL,0);
            break;
        case RAWDATA_MODE:
            ok = hyn_wr_reg(hyn_92xxdata,0xD10A,2,NULL,0);
            break;
        case FAC_TEST_MODE:
            hyn_wr_reg(hyn_92xxdata,0xD114,2,NULL,0);
            msleep(50);
            break;
        case CHARGE_EXIT:
        case CHARGE_ENTER:
            hyn_92xxdata->charge_is_enable = mode&0x01;
            ok = hyn_wr_reg(hyn_92xxdata,(mode&0x01)? 0xD01F:0xD020,2,0,0); //charg mode
            mode = hyn_92xxdata->work_mode; //not switch work mode
            HYN_INFO("set_charge:%d",hyn_92xxdata->charge_is_enable);
            break;
        case ENTER_BOOT_MODE:
            ok = cst92xx_enter_boot();
            break;
        case DEEPSLEEP:
            hyn_wr_reg(hyn_92xxdata,0xD105,2,NULL,0);
            break;
        default :
            hyn_92xxdata->work_mode = NOMAL_MODE;
            ok = -2;
            break;
    }
    if(ok != -2){
        hyn_92xxdata->work_mode = mode;
    }
    HYN_INFO("set_workmode:%d ret:%d",mode,ok);
    return ok;
}

static void cst92xx_rst(void)
{
    if(hyn_92xxdata->work_mode==ENTER_BOOT_MODE){
        hyn_set_i2c_addr(hyn_92xxdata,MAIN_I2C_ADDR);
    }
    gpio_set_value(hyn_92xxdata->plat_data.reset_gpio,0);
    msleep(8);
    gpio_set_value(hyn_92xxdata->plat_data.reset_gpio,1);
}

static int cst92xx_supend(void)
{
    HYN_ENTER();
    cst92xx_set_workmode(DEEPSLEEP,0);
    return 0;
}

static int cst92xx_resum(void)
{
    cst92xx_rst();
    msleep(50);
    cst92xx_set_workmode(NOMAL_MODE,1);
    return 0;
}


static int cst92xx_report(void)
{
    int ret = FALSE;
    uint8_t i2c_buf[MAX_FINGER*5+5] = {0};
    uint8_t finger_num = 0,key_state=0,retry=3,re_len=0,ges_state;

    while(--retry){
        hyn_wr_reg(hyn_92xxdata,0xD000,2,NULL,0); //wakeup
        udelay(200);
        ret = hyn_wr_reg(hyn_92xxdata,0xD000,2,i2c_buf,7);
        if(ret || i2c_buf[6]!=0xAB || i2c_buf[0]==0xAB){
            ret = -1;
            continue;
        }
        finger_num = i2c_buf[5] & 0x7F;
        if(finger_num > MAX_FINGER){
            ret = -2;
            continue;
        }
        key_state = i2c_buf[5]>>7;
        re_len = ((finger_num>1 ? finger_num-1:0)+key_state)*5;
        if(re_len){
            ret = hyn_wr_reg(hyn_92xxdata,0xD007,2,i2c_buf+7,re_len);
            if(ret){
                ret = -2;
                continue;
            }
        }
        ret = hyn_wr_reg(hyn_92xxdata,0xD000AB,3,NULL,0); //write end flag
        if(ret){
            ret = -3;
            continue;
        }
        else{
            break;
        }
    }
    
    if(ret){
        HYN_ERROR("read point erro:%d\r\n",ret);
        hyn_wr_reg(hyn_92xxdata,0xD000AB,3,NULL,0); //write end flag
        return ret;
    }

    ges_state = i2c_buf[4]>>4;
    if(ges_state){ //gesture
        if((ges_state&0x80) == 0x80){ //palm
            hyn_92xxdata->gesture_id = IDX_POWER;//IDX_Z;// palm 
        }
        else{ //other gesture
            hyn_92xxdata->gesture_id = IDX_POWER;//GESTURE wakeup
        }
        HYN_INFO("gesture buf4:%#x\r\n",i2c_buf[4]);
        hyn_92xxdata->rp_buf.report_need |= REPORT_GES;
    }

    if(key_state){
        uint8_t *data_ptr = i2c_buf;
        if(finger_num){
            data_ptr += (finger_num*5+2);
        }
        if( hyn_92xxdata->rp_buf.report_need==REPORT_NONE){
            hyn_92xxdata->rp_buf.report_need |= REPORT_KEY;
        }
        hyn_92xxdata->rp_buf.key_id = (data_ptr[1]>>4)-1; //0x17  0x27  0x37
        hyn_92xxdata->rp_buf.key_state = data_ptr[0]==0x83 ? 1:0; //0x83  0x80
    }

    if(finger_num){
        u8 i = 0,index = 0,id = 0;
        u8 *data_ptr = i2c_buf;
        if( hyn_92xxdata->rp_buf.report_need==REPORT_NONE){
            hyn_92xxdata->rp_buf.report_need |= REPORT_POS;
        }
        for (i = 0; i < finger_num; i++) {
            id = data_ptr[0] >> 4;
            hyn_92xxdata->rp_buf.pos_info[index].pos_id = id;
            hyn_92xxdata->rp_buf.pos_info[index].event = (data_ptr[0]&0x0F) ? 1 : 0;  
            hyn_92xxdata->rp_buf.pos_info[index].pos_x = (data_ptr[1] << 4) | (data_ptr[3] >> 4);
            hyn_92xxdata->rp_buf.pos_info[index].pos_y = (data_ptr[2] << 4) | (data_ptr[3] & 0x0F);
            hyn_92xxdata->rp_buf.pos_info[index].pres_z = (data_ptr[4] & 0x7F);
            index++;
            data_ptr += (i==0 ? 7:5);
        }
        hyn_92xxdata->rp_buf.rep_num = index;
    }

    return TRUE;
}


static u32 cst92xx_check_esd(void)
{
    int ok = FALSE;
    uint8_t i2c_buf[6], retry;
    uint32_t esd_value = 0;
    retry = 4;
    while (retry--){
        hyn_wr_reg(hyn_92xxdata,0xD048,2,i2c_buf,0);
        udelay(200);
        ok = hyn_wr_reg(hyn_92xxdata,0xD048,2,i2c_buf,1);
        if (ok == 0 &&((i2c_buf[0]&0xF0) == 0x20 || (i2c_buf[0]&0xF0) == 0xA0)&&(i2c_buf[0]&0x0F)<10){
            break;
        }
        else{
            ok = -2;
        }
    }
    if(-2==ok){   //esdcheck failed rest system
        hyn_92xxdata->esd_fail_cnt = 3;
        esd_value = hyn_92xxdata->esd_last_value;
    }
    else{
        esd_value = hyn_92xxdata->esd_last_value+1;
    }
    return esd_value;
}


static int cst92xx_prox_handle(u8 cmd)
{
    return TRUE;
}


static int cst92xx_get_dbg_data(u8 *buf, u16 len)
{
    int ret = -1;  
    u16 read_len = (hyn_92xxdata->hw_info.fw_sensor_txnum * hyn_92xxdata->hw_info.fw_sensor_rxnum)*2;
    u16 total_len = read_len + (hyn_92xxdata->hw_info.fw_sensor_txnum + hyn_92xxdata->hw_info.fw_sensor_rxnum)*2;
    HYN_ENTER();
    if(total_len > len){
        HYN_ERROR("buf too small");
        return -1;
    }
    switch(hyn_92xxdata->work_mode){
        case DIFF_MODE:
        case RAWDATA_MODE:
            ret = hyn_wr_reg(hyn_92xxdata,0x1000,2,buf,read_len); //mt 

            buf += read_len;
            read_len = hyn_92xxdata->hw_info.fw_sensor_rxnum*2;
            ret |= hyn_wr_reg(hyn_92xxdata,0x7000,2,buf,read_len); //rx

            buf += read_len;
            read_len = hyn_92xxdata->hw_info.fw_sensor_txnum*2;
            ret |= hyn_wr_reg(hyn_92xxdata,0x7200,2,buf,read_len); //tx

            ret |= hyn_wr_reg(hyn_92xxdata,0x000500,3,0,0); //end
            break;
        default:
            HYN_ERROR("work_mode:%d",hyn_92xxdata->work_mode);
            break;
    }
    return ret==0 ? total_len:-1;
}


static int cst92xx_get_test_result(u8 *buf, u16 len)
{
    int ret = 0;
    struct tp_info *ic = &hyn_92xxdata->hw_info;
    u16 scap_len = (ic->fw_sensor_txnum + ic->fw_sensor_rxnum)*2;
    u16 mt_len = ic->fw_sensor_rxnum*ic->fw_sensor_txnum*2,i = 0;
    u16 *raw_s;

    HYN_ENTER();
    if((mt_len*3 + scap_len) > len || mt_len==0){
        HYN_ERROR("buf too small");
        return FAC_GET_DATA_FAIL;
    }
    HYN_INFO("---open_higdrv---");
    hyn_wr_reg(hyn_92xxdata,0xD110,2,buf,0); ////test open high
    hyn_wait_irq_timeout(hyn_92xxdata,7000);
    if(hyn_wr_reg(hyn_92xxdata,0x3000,2,buf,mt_len)){ //open high
        ret = FAC_GET_DATA_FAIL;
        HYN_ERROR("read open high failed");
        goto selftest_end;
    }
    hyn_wr_reg(hyn_92xxdata,0x000005,3,buf,0); 

    HYN_INFO("---open_low---");
    hyn_wr_reg(hyn_92xxdata,0xD111,2,buf,0); ////test open low
    hyn_wait_irq_timeout(hyn_92xxdata,7000);
    if(hyn_wr_reg(hyn_92xxdata,0x1000,2,buf + mt_len,mt_len)){ //open low
        ret = FAC_GET_DATA_FAIL;
        HYN_ERROR("read open low failed");
        goto selftest_end;
    }
    hyn_wr_reg(hyn_92xxdata,0x000005,3,buf,0); 

    //short test
    HYN_INFO("---short---");
    hyn_wr_reg(hyn_92xxdata,0xD112,2,buf,0); //// short test
    hyn_wait_irq_timeout(hyn_92xxdata,7000);
    if(hyn_wr_reg(hyn_92xxdata,0x5000,2,buf+(mt_len*2),scap_len)){
        ret = FAC_GET_DATA_FAIL;
        HYN_ERROR("read fac short failed");
        goto selftest_end;
    }
    else{
        raw_s = (u16*)(buf + mt_len*2);
         HYN_INFO("raw_s start data =  %d",*(raw_s));
        for(i = 0; i< ic->fw_sensor_rxnum+ic->fw_sensor_txnum; i++){
            HYN_INFO("short raw data = %d %d",i,*(raw_s+i));
            if(U16REV((u16)*raw_s) != 0)  *raw_s = 2000 / U16REV((u16)*raw_s);
            else  *raw_s =0;
            HYN_INFO("short reprocess data = %d %d",i,*(raw_s+i));
            raw_s++;
        }
    }
selftest_end:
    cst92xx_resum();
    return ret;
}

const struct hyn_ts_fuc cst92xx_fuc = {
    .tp_rest = cst92xx_rst,
    .tp_report = cst92xx_report,
    .tp_supend = cst92xx_supend,
    .tp_resum = cst92xx_resum,
    .tp_chip_init = cst92xx_init,
    .tp_updata_fw = cst92xx_updata_fw,
    .tp_set_workmode = cst92xx_set_workmode,
    .tp_check_esd = cst92xx_check_esd,
    .tp_prox_handle = cst92xx_prox_handle,
    .tp_get_dbg_data = cst92xx_get_dbg_data,
    .tp_get_test_result = cst92xx_get_test_result
};


