/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#ifdef RTE_IBVERBS_LINK_DLOPEN
#include <dlfcn.h>
#endif
#include <dirent.h>
#include <net/if.h>

#include <rte_errno.h>
#include <rte_string_fns.h>

#include "mlx5_common.h"
#include "mlx5_common_log.h"
#include "mlx5_common_defs.h"
#include "mlx5_common_os.h"
#include "mlx5_glue.h"

#ifdef MLX5_GLUE
const struct mlx5_glue *mlx5_glue;
#endif

int
mlx5_get_pci_addr(const char *dev_path, struct rte_pci_addr *pci_addr)
{
	FILE *file;
	char line[32];
	int rc = -ENOENT;
	MKSTR(path, "%s/device/uevent", dev_path);

	file = fopen(path, "rb");
	if (file == NULL) {
		rte_errno = errno;
		return -rte_errno;
	}
	while (fgets(line, sizeof(line), file) == line) {
		size_t len = strlen(line);

		/* Truncate long lines. */
		if (len == (sizeof(line) - 1)) {
			while (line[(len - 1)] != '\n') {
				int ret = fgetc(file);

				if (ret == EOF)
					goto exit;
				line[(len - 1)] = ret;
			}
			/* No match for long lines. */
			continue;
		}
		/* Extract information. */
		if (sscanf(line,
			   "PCI_SLOT_NAME="
			   "%" SCNx32 ":%" SCNx8 ":%" SCNx8 ".%" SCNx8 "\n",
			   &pci_addr->domain,
			   &pci_addr->bus,
			   &pci_addr->devid,
			   &pci_addr->function) == 4) {
			rc = 0;
			break;
		}
	}
exit:
	fclose(file);
	if (rc)
		rte_errno = -rc;
	return rc;
}

/**
 * Extract port name, as a number, from sysfs or netlink information.
 *
 * @param[in] port_name_in
 *   String representing the port name.
 * @param[out] port_info_out
 *   Port information, including port name as a number and port name
 *   type if recognized
 *
 * @return
 *   port_name field set according to recognized name format.
 */
void
mlx5_translate_port_name(const char *port_name_in,
			 struct mlx5_switch_info *port_info_out)
{
	char ctrl = 0, pf_c1, pf_c2, vf_c1, vf_c2, eol;
	char *end;
	int sc_items;

	sc_items = sscanf(port_name_in, "%c%d",
			  &ctrl, &port_info_out->ctrl_num);
	if (sc_items == 2 && ctrl == 'c') {
		port_name_in++; /* 'c' */
		port_name_in += snprintf(NULL, 0, "%d",
					  port_info_out->ctrl_num);
	}
	/* Check for port-name as a string of the form pf0vf0 or pf0sf0 */
	sc_items = sscanf(port_name_in, "%c%c%d%c%c%d%c",
			  &pf_c1, &pf_c2, &port_info_out->pf_num,
			  &vf_c1, &vf_c2, &port_info_out->port_name, &eol);
	if (sc_items == 6 && pf_c1 == 'p' && pf_c2 == 'f') {
		if (vf_c1 == 'v' && vf_c2 == 'f') {
			/* Kernel ver >= 5.0 or OFED ver >= 4.6 */
			port_info_out->name_type =
					MLX5_PHYS_PORT_NAME_TYPE_PFVF;
			return;
		}
		if (vf_c1 == 's' && vf_c2 == 'f') {
			/* Kernel ver >= 5.11 or OFED ver >= 5.1 */
			port_info_out->name_type =
					MLX5_PHYS_PORT_NAME_TYPE_PFSF;
			return;
		}
	}
	/*
	 * Check for port-name as a string of the form p0
	 * (support kernel ver >= 5.0, or OFED ver >= 4.6).
	 */
	sc_items = sscanf(port_name_in, "%c%d%c",
			  &pf_c1, &port_info_out->port_name, &eol);
	if (sc_items == 2 && pf_c1 == 'p') {
		port_info_out->name_type = MLX5_PHYS_PORT_NAME_TYPE_UPLINK;
		return;
	}
	/*
	 * Check for port-name as a string of the form pf0
	 * (support kernel ver >= 5.7 for HPF representor on BF).
	 */
	sc_items = sscanf(port_name_in, "%c%c%d%c",
			  &pf_c1, &pf_c2, &port_info_out->pf_num, &eol);
	if (sc_items == 3 && pf_c1 == 'p' && pf_c2 == 'f') {
		port_info_out->port_name = -1;
		port_info_out->name_type = MLX5_PHYS_PORT_NAME_TYPE_PFHPF;
		return;
	}
	/* Check for port-name as a number (support kernel ver < 5.0 */
	errno = 0;
	port_info_out->port_name = strtol(port_name_in, &end, 0);
	if (!errno &&
	    (size_t)(end - port_name_in) == strlen(port_name_in)) {
		port_info_out->name_type = MLX5_PHYS_PORT_NAME_TYPE_LEGACY;
		return;
	}
	port_info_out->name_type = MLX5_PHYS_PORT_NAME_TYPE_UNKNOWN;
}

int
mlx5_get_ifname_sysfs(const char *ibdev_path, char *ifname)
{
	DIR *dir;
	struct dirent *dent;
	unsigned int dev_type = 0;
	unsigned int dev_port_prev = ~0u;
	char match[IF_NAMESIZE] = "";

	MLX5_ASSERT(ibdev_path);
	{
		MKSTR(path, "%s/device/net", ibdev_path);

		dir = opendir(path);
		if (dir == NULL) {
			rte_errno = errno;
			return -rte_errno;
		}
	}
	while ((dent = readdir(dir)) != NULL) {
		char *name = dent->d_name;
		FILE *file;
		unsigned int dev_port;
		int r;

		if ((name[0] == '.') &&
		    ((name[1] == '\0') ||
		     ((name[1] == '.') && (name[2] == '\0'))))
			continue;

		MKSTR(path, "%s/device/net/%s/%s",
		      ibdev_path, name,
		      (dev_type ? "dev_id" : "dev_port"));

		file = fopen(path, "rb");
		if (file == NULL) {
			if (errno != ENOENT)
				continue;
			/*
			 * Switch to dev_id when dev_port does not exist as
			 * is the case with Linux kernel versions < 3.15.
			 */
try_dev_id:
			match[0] = '\0';
			if (dev_type)
				break;
			dev_type = 1;
			dev_port_prev = ~0u;
			rewinddir(dir);
			continue;
		}
		r = fscanf(file, (dev_type ? "%x" : "%u"), &dev_port);
		fclose(file);
		if (r != 1)
			continue;
		/*
		 * Switch to dev_id when dev_port returns the same value for
		 * all ports. May happen when using a MOFED release older than
		 * 3.0 with a Linux kernel >= 3.15.
		 */
		if (dev_port == dev_port_prev)
			goto try_dev_id;
		dev_port_prev = dev_port;
		if (dev_port == 0)
			strlcpy(match, name, IF_NAMESIZE);
	}
	closedir(dir);
	if (match[0] == '\0') {
		rte_errno = ENOENT;
		return -rte_errno;
	}
	strncpy(ifname, match, IF_NAMESIZE);
	return 0;
}

#ifdef MLX5_GLUE

/**
 * Suffix RTE_EAL_PMD_PATH with "-glue".
 *
 * This function performs a sanity check on RTE_EAL_PMD_PATH before
 * suffixing its last component.
 *
 * @param buf[out]
 *   Output buffer, should be large enough otherwise NULL is returned.
 * @param size
 *   Size of @p out.
 *
 * @return
 *   Pointer to @p buf or @p NULL in case suffix cannot be appended.
 */
static char *
mlx5_glue_path(char *buf, size_t size)
{
	static const char *const bad[] = { "/", ".", "..", NULL };
	const char *path = RTE_EAL_PMD_PATH;
	size_t len = strlen(path);
	size_t off;
	int i;

	while (len && path[len - 1] == '/')
		--len;
	for (off = len; off && path[off - 1] != '/'; --off)
		;
	for (i = 0; bad[i]; ++i)
		if (!strncmp(path + off, bad[i], (int)(len - off)))
			goto error;
	i = snprintf(buf, size, "%.*s-glue", (int)len, path);
	if (i == -1 || (size_t)i >= size)
		goto error;
	return buf;
error:
	RTE_LOG(ERR, PMD, "unable to append \"-glue\" to last component of"
		" RTE_EAL_PMD_PATH (\"" RTE_EAL_PMD_PATH "\"), please"
		" re-configure DPDK");
	return NULL;
}

static int
mlx5_glue_dlopen(void)
{
	char glue_path[sizeof(RTE_EAL_PMD_PATH) - 1 + sizeof("-glue")];
	void *handle = NULL;

	char const *path[] = {
		/*
		 * A basic security check is necessary before trusting
		 * MLX5_GLUE_PATH, which may override RTE_EAL_PMD_PATH.
		 */
		(geteuid() == getuid() && getegid() == getgid() ?
		 getenv("MLX5_GLUE_PATH") : NULL),
		/*
		 * When RTE_EAL_PMD_PATH is set, use its glue-suffixed
		 * variant, otherwise let dlopen() look up libraries on its
		 * own.
		 */
		(*RTE_EAL_PMD_PATH ?
		 mlx5_glue_path(glue_path, sizeof(glue_path)) : ""),
	};
	unsigned int i = 0;
	void **sym;
	const char *dlmsg;

	while (!handle && i != RTE_DIM(path)) {
		const char *end;
		size_t len;
		int ret;

		if (!path[i]) {
			++i;
			continue;
		}
		end = strpbrk(path[i], ":;");
		if (!end)
			end = path[i] + strlen(path[i]);
		len = end - path[i];
		ret = 0;
		do {
			char name[ret + 1];

			ret = snprintf(name, sizeof(name), "%.*s%s" MLX5_GLUE,
				       (int)len, path[i],
				       (!len || *(end - 1) == '/') ? "" : "/");
			if (ret == -1)
				break;
			if (sizeof(name) != (size_t)ret + 1)
				continue;
			DRV_LOG(DEBUG, "Looking for rdma-core glue as "
				"\"%s\"", name);
			handle = dlopen(name, RTLD_LAZY);
			break;
		} while (1);
		path[i] = end + 1;
		if (!*end)
			++i;
	}
	if (!handle) {
		rte_errno = EINVAL;
		dlmsg = dlerror();
		if (dlmsg)
			DRV_LOG(WARNING, "Cannot load glue library: %s", dlmsg);
		goto glue_error;
	}
	sym = dlsym(handle, "mlx5_glue");
	if (!sym || !*sym) {
		rte_errno = EINVAL;
		dlmsg = dlerror();
		if (dlmsg)
			DRV_LOG(ERR, "Cannot resolve glue symbol: %s", dlmsg);
		goto glue_error;
	}
	mlx5_glue = *sym;
	return 0;

glue_error:
	if (handle)
		dlclose(handle);
	return -1;
}

#endif

/**
 * Initialization routine for run-time dependency on rdma-core.
 */
void
mlx5_glue_constructor(void)
{
	/*
	 * RDMAV_HUGEPAGES_SAFE tells ibv_fork_init() we intend to use
	 * huge pages. Calling ibv_fork_init() during init allows
	 * applications to use fork() safely for purposes other than
	 * using this PMD, which is not supported in forked processes.
	 */
	setenv("RDMAV_HUGEPAGES_SAFE", "1", 1);
	/* Match the size of Rx completion entry to the size of a cacheline. */
	if (RTE_CACHE_LINE_SIZE == 128)
		setenv("MLX5_CQE_SIZE", "128", 0);
	/*
	 * MLX5_DEVICE_FATAL_CLEANUP tells ibv_destroy functions to
	 * cleanup all the Verbs resources even when the device was removed.
	 */
	setenv("MLX5_DEVICE_FATAL_CLEANUP", "1", 1);

#ifdef MLX5_GLUE
	if (mlx5_glue_dlopen() != 0)
		goto glue_error;
#endif

#ifdef RTE_LIBRTE_MLX5_DEBUG
	/* Glue structure must not contain any NULL pointers. */
	{
		unsigned int i;

		for (i = 0; i != sizeof(*mlx5_glue) / sizeof(void *); ++i)
			MLX5_ASSERT(((const void *const *)mlx5_glue)[i]);
	}
#endif
	if (strcmp(mlx5_glue->version, MLX5_GLUE_VERSION)) {
		rte_errno = EINVAL;
		DRV_LOG(ERR, "rdma-core glue \"%s\" mismatch: \"%s\" is "
			"required", mlx5_glue->version, MLX5_GLUE_VERSION);
		goto glue_error;
	}
	mlx5_glue->fork_init();
	return;

glue_error:
	DRV_LOG(WARNING, "Cannot initialize MLX5 common due to missing"
		" run-time dependency on rdma-core libraries (libibverbs,"
		" libmlx5)");
	mlx5_glue = NULL;
}

struct ibv_device *
mlx5_os_get_ibv_device(const struct rte_pci_addr *addr)
{
	int n;
	struct ibv_device **ibv_list = mlx5_glue->get_device_list(&n);
	struct ibv_device *ibv_match = NULL;

	if (ibv_list == NULL) {
		rte_errno = ENOSYS;
		return NULL;
	}
	while (n-- > 0) {
		struct rte_pci_addr paddr;

		DRV_LOG(DEBUG, "Checking device \"%s\"..", ibv_list[n]->name);
		if (mlx5_get_pci_addr(ibv_list[n]->ibdev_path, &paddr) != 0)
			continue;
		if (rte_pci_addr_cmp(addr, &paddr) != 0)
			continue;
		ibv_match = ibv_list[n];
		break;
	}
	if (ibv_match == NULL)
		rte_errno = ENOENT;
	mlx5_glue->free_device_list(ibv_list);
	return ibv_match;
}

static int
mlx5_config_doorbell_mapping_env(int dbnc)
{
	char *env;
	int value;

	MLX5_ASSERT(rte_eal_process_type() == RTE_PROC_PRIMARY);
	/* Get environment variable to store. */
	env = getenv(MLX5_SHUT_UP_BF);
	value = env ? !!strcmp(env, "0") : MLX5_ARG_UNSET;
	if (dbnc == MLX5_ARG_UNSET)
		setenv(MLX5_SHUT_UP_BF, MLX5_SHUT_UP_BF_DEFAULT, 1);
	else
		setenv(MLX5_SHUT_UP_BF,
		       dbnc == MLX5_TXDB_NCACHED ? "1" : "0", 1);
	return value;
}

static void
mlx5_restore_doorbell_mapping_env(int value)
{
	MLX5_ASSERT(rte_eal_process_type() == RTE_PROC_PRIMARY);
	/* Restore the original environment variable state. */
	if (value == MLX5_ARG_UNSET)
		unsetenv(MLX5_SHUT_UP_BF);
	else
		setenv(MLX5_SHUT_UP_BF, value ? "1" : "0", 1);
}

/**
 * Function API to open IB device.
 *
 *
 * @param cdev
 *   Pointer to the mlx5 device.
 * @param ctx_ptr
 *   Pointer to fill inside pointer to device context.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
int
mlx5_os_open_device(struct mlx5_common_device *cdev, void **ctx_ptr)
{
	struct ibv_device *ibv;
	struct ibv_context *ctx = NULL;
	int dbmap_env;

	ibv = mlx5_os_get_ibv_dev(cdev->dev);
	if (!ibv)
		return -rte_errno;
	DRV_LOG(INFO, "Dev information matches for device \"%s\".", ibv->name);
	/*
	 * Configure environment variable "MLX5_BF_SHUT_UP" before the device
	 * creation. The rdma_core library checks the variable at device
	 * creation and stores the result internally.
	 */
	dbmap_env = mlx5_config_doorbell_mapping_env(cdev->config.dbnc);
	/* Try to open IB device with DV first, then usual Verbs. */
	errno = 0;
	ctx = mlx5_glue->dv_open_device(ibv);
	if (ctx) {
		cdev->config.devx = 1;
		DRV_LOG(DEBUG, "DevX is supported.");
	} else {
		/* The environment variable is still configured. */
		ctx = mlx5_glue->open_device(ibv);
		if (ctx == NULL)
			goto error;
		DRV_LOG(DEBUG, "DevX is NOT supported.");
	}
	/* The device is created, no need for environment. */
	mlx5_restore_doorbell_mapping_env(dbmap_env);
	/* Hint libmlx5 to use PMD allocator for data plane resources */
	mlx5_set_context_attr(cdev->dev, ctx);
	*ctx_ptr = (void *)ctx;
	return 0;
error:
	rte_errno = errno ? errno : ENODEV;
	/* The device creation is failed, no need for environment. */
	mlx5_restore_doorbell_mapping_env(dbmap_env);
	DRV_LOG(ERR, "Failed to open IB device \"%s\".", ibv->name);
	return -rte_errno;
}
