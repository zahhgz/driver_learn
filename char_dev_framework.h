#ifndef __CHAR_DEV_FRAMEWORK_H__
#define __CHAR_DEV_FRAMEWORK_H__

#include <linux/ioctl.h>

#define CDF_DEV_NAME        "cdf_dev"
#define CDF_CLASS_NAME      "cdf_class"
#define CDF_DEV_NUM         3
#define CDF_BUF_SIZE        1024

#define CDF_MAGIC           'c'

#define CDF_CMD_GET_STATUS      _IOR(CDF_MAGIC, 0x01, int)
#define CDF_CMD_SET_PARAM       _IOW(CDF_MAGIC, 0x02, int)
#define CDF_CMD_CLEAR_BUF       _IO(CDF_MAGIC, 0x03)
#define CDF_CMD_GET_BUF_SIZE    _IOR(CDF_MAGIC, 0x04, int)

#define CDF_CMD_MAX             0x05

#endif
