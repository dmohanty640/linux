# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Hisilicon RoCE drivers.
#

ccflags-y :=  -I $(srctree)/drivers/net/ethernet/hisilicon/hns3
ccflags-y +=  -I $(src)

hns-roce-hw-v2-objs := hns_roce_main.o hns_roce_cmd.o hns_roce_pd.o \
	hns_roce_ah.o hns_roce_hem.o hns_roce_mr.o hns_roce_qp.o \
	hns_roce_cq.o hns_roce_alloc.o hns_roce_db.o hns_roce_srq.o hns_roce_restrack.o \
	hns_roce_debugfs.o hns_roce_hw_v2.o

obj-$(CONFIG_INFINIBAND_HNS_HIP08) += hns-roce-hw-v2.o
