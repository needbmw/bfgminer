#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "logging.h"
#include "spidevc.h"
#include "tm_i2c.h"

#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1 ignores bits which are 0


//static int gpio_map[32] = { 2, 3, 4, 14, 15, 17, 18, 27, 22, 23, 24, 25, 8, 7};
static int gpio_map[] = { 7, 8, 25, 24, 23, 22, 27, 18, 17, 4, 3, 2 }; //new RasPi and rev 0b2 backplane only

#define RACK_SLOTS (sizeof(gpio_map)/sizeof(int))

static int tm_i2c_fd;

float tm_i2c_Data2Temp(unsigned int ans) {
	float t = ans;
	return (t / 1023.0 * 3.3 * 2-2.73) * 100.0;
}

float tm_i2c_Data2Core(unsigned int ans) {
	float t = ans;
	return t / 1023.0 * 3.3;
}

int tm_i2c_init() {
	int i;

	spi_init();

	if ((tm_i2c_fd = open("/dev/i2c-1", O_RDWR)) < 0)
		;
//		return 1;
//	else
//		return 0;

	for(i=0; i<RACK_SLOTS; i++) {
		gpio_inp(gpio_map[i]);
		gpio_out(gpio_map[i]);
//		gpio_set_alt(gpio_map[i], 0);
		gpio_set(gpio_map[i]);
	}

	return 0;
}

void tm_i2c_close() {
	close(tm_i2c_fd);
}

unsigned int tm_i2c_req(int fd, unsigned char addr, unsigned char cmd, unsigned int data) {
	int i;
	unsigned char buf[16];
	struct i2c_msg msg;
	tm_struct *tm = (tm_struct *) buf;
	struct i2c_rdwr_ioctl_data msg_rdwr;
	unsigned int ret;

	//applog(LOG_DEBUG, "REQ from %02X cmd: %02X", addr, cmd);

	tm->cmd = cmd;
	tm->data_lsb = data & 0xFF;
	tm->data_msb = (data & 0xFF00) >> 8;

	/* Write CMD */
	msg.addr = addr;
	msg.flags = 0;
	msg.len = 3;
	msg.buf = buf;
	msg_rdwr.msgs = &msg;
	msg_rdwr.nmsgs = 1;
	if ((i = ioctl(fd, I2C_RDWR, &msg_rdwr)) < 0) {
//		perror("ioctl error");
		return -1;
	}

	/* Read result */
	msg.addr = addr;
	msg.flags = I2C_M_RD;
	msg.len = 3;
	msg.buf = buf;
	msg_rdwr.msgs = &msg;
	msg_rdwr.nmsgs = 1;
	if ((i = ioctl(fd, I2C_RDWR, &msg_rdwr)) < 0) {
//		perror("ioctl error");
		return -1;
	}

	//hexdump(buf, 10);
	ret = (tm->data_msb << 8) + tm->data_lsb;
	if (tm->cmd == cmd) return ret;
	return 0;
}

int tm_i2c_detect(unsigned char slot) {
	if (slot < 0 || slot > 31) return 0;
	return tm_i2c_req(tm_i2c_fd, (TM_ADDR >> 1) + slot, TM_GET_CORE0, 0);
}

float tm_i2c_getcore0(unsigned char slot) {
	if (slot < 0 || slot > 31) return 0;
	return tm_i2c_Data2Core(tm_i2c_req(tm_i2c_fd, (TM_ADDR >> 1) + slot, TM_GET_CORE0, 0));
}

float tm_i2c_getcore1(unsigned char slot) {
	if (slot < 0 || slot > 31) return 0;
	return tm_i2c_Data2Core(tm_i2c_req(tm_i2c_fd, (TM_ADDR >> 1) + slot, TM_GET_CORE1, 0));
}

float tm_i2c_gettemp(unsigned char slot) {
	if (slot < 0 || slot > 31) return 0;
	return tm_i2c_Data2Temp(tm_i2c_req(tm_i2c_fd, (TM_ADDR >> 1) + slot, TM_GET_TEMP, 0));
}

void tm_i2c_set_oe(unsigned char slot) {

	if (slot < 0 || slot >= RACK_SLOTS) return;
#if 0
	tm_i2c_req(tm_i2c_fd, (TM_ADDR >> 1) + slot, TM_SET_OE, 0);
#else
	gpio_inp(gpio_map[slot]);
	gpio_out(gpio_map[slot]);
	gpio_clr(gpio_map[slot]);
#endif
}

void tm_i2c_clear_oe(unsigned char slot) {

	if (slot < 0 || slot >= RACK_SLOTS) return;
#if 0
	tm_i2c_req(tm_i2c_fd, (TM_ADDR >> 1) + slot, TM_SET_OE, 1);
#else
	gpio_inp(gpio_map[slot]);
	gpio_out(gpio_map[slot]);
	gpio_set(gpio_map[slot]);
#endif
}

unsigned char tm_i2c_slot2addr(unsigned char slot) {
	if (slot < 0 || slot > 31) return 0;
	return ((TM_ADDR >> 1) + slot);
}

