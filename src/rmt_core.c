
#include <rmt_core.h>

#include <time.h>
#include <signal.h>

static void recv_data_from_target(aeEventLoop *el, int fd, void *privdata, int mask);
static void send_data_to_target(aeEventLoop *el, int fd, void *privdata, int mask);
static int readThreadCron(struct aeEventLoop *eventLoop, long long id, void *clientData);
static int writeThreadCron(struct aeEventLoop *eventLoop, long long id, void *clientData);

static int read_thread_data_init(read_thread_data *rdata)
{
	if (rdata == NULL) {
		return RMT_ERROR;
	}

    rdata->id = 0;
	rdata->thread_id = 0;
    rdata->finish_read_nodes = 0;
    rdata->nodes_count = 0;
    rdata->loop = NULL;
    rdata->unixtime = rmt_msec_now();
    rdata->nodes_data = NULL;
    rdata->stat_total_net_input_bytes = 0;

    rdata->loop = aeCreateEventLoop(1000);
    if (rdata->loop == NULL) {
    	log_error("ERROR: create event loop failed");
        return RMT_ERROR;
    }
    
	rdata->nodes_data = listCreate();
	if (rdata->nodes_data == NULL) {
		log_error("ERROR: create node list failed: out of memory");
		return RMT_ENOMEM;
	}

    if (aeCreateTimeEvent(rdata->loop, 1, readThreadCron, rdata, NULL) == AE_ERR) {
        log_error("ERROR: can't create the readThreadCron time event.");
        return RMT_ERROR;
    }
	
	return RMT_OK;
}

static void read_thread_data_deinit(read_thread_data *rdata)
{
	if (rdata == NULL) {
		return;
	}

    if (rdata->loop != NULL) {
		aeDeleteEventLoop(rdata->loop);
		rdata->loop = NULL;
	}

	if (rdata->nodes_data != NULL) {
		listRelease(rdata->nodes_data);
		rdata->nodes_data = NULL;
	}
}

static void write_thread_data_deinit(write_thread_data *wdata);

static int write_thread_data_init(rmtContext *ctx, write_thread_data *wdata)
{
    int ret;
    dictIterator *di;
    dictEntry *de;
    redis_node *trnode;
    
	if (wdata == NULL) {
		return RMT_ERROR;
	}

    wdata->id = 0;
	wdata->thread_id = 0;
    wdata->finish_write_nodes = 0;
    wdata->nodes_count = 0;
	wdata->loop = NULL;
    wdata->unixtime = rmt_msec_now();
    wdata->trgroup = NULL;
    wdata->nodes = NULL;
    wdata->notice_pipe[0] = -1;
    wdata->notice_pipe[1] = -1;
    wdata->stat_total_msgs_recv = 0;
    wdata->stat_total_msgs_sent = 0;
    wdata->stat_total_net_output_bytes = 0;
    wdata->stat_rdb_parsed_count = 0;
    wdata->stat_mbufs_inqueue = 0;    
    wdata->stat_msgs_outqueue = 0;

	wdata->loop = aeCreateEventLoop(1000);
    if (wdata->loop == NULL) {
    	log_error("ERROR:  create event loop failed");
        goto error;
    }

    wdata->nodes = listCreate();
	if (wdata->nodes == NULL) {
		log_error("ERROR: Create node list failed: out of memory");
		goto error;
	}

    wdata->trgroup = target_group_create(ctx);
    if (wdata->trgroup == NULL) {
        log_error("ERROR: Target group create failed");
        goto error;
    }

    di = dictGetSafeIterator(wdata->trgroup->nodes);
    while ((de = dictNext(di)) != NULL) {
        trnode = dictGetVal(de);
        trnode->write_data = wdata;
    }
    dictReleaseIterator(di);

    ret = pipe(wdata->notice_pipe);
    if (ret < 0) {
        log_error("ERROR: Notice_pipe init failed: %s", strerror(errno));
        goto error;
    }

    ret = rmt_set_nonblocking(wdata->notice_pipe[0]);
    if (ret < 0) {
        log_error("ERROR: Set notice_pipe[0] %d nonblock failed: %s", 
            wdata->notice_pipe[0], strerror(errno));
        goto error;
    }

    ret = rmt_set_nonblocking(wdata->notice_pipe[1]);
    if (ret < 0){
        log_error("ERROR: Set notice_pipe[1] %d nonblock failed: %s", 
            wdata->notice_pipe[1], strerror(errno));
        goto error;
    }

    if (aeCreateTimeEvent(wdata->loop, 1, writeThreadCron, wdata, NULL) == AE_ERR) {
        log_error("ERROR: can't create the writeThreadCron time event.");
        goto error;
    }
	
	return RMT_OK;

error:

    write_thread_data_deinit(wdata);
    return RMT_ERROR;
}

static void write_thread_data_deinit(write_thread_data *wdata)
{
	if(wdata == NULL)
	{
		return;
	}

	if(wdata->loop != NULL)
	{
		aeDeleteEventLoop(wdata->loop);
		wdata->loop = NULL;
	}

    if(wdata->nodes != NULL)
	{
		listRelease(wdata->nodes);
		wdata->nodes = NULL;
	}

    if(wdata->trgroup != NULL){
        target_group_destroy(wdata->trgroup);
        wdata->trgroup = NULL;
    }

    if(wdata->notice_pipe[0] > 0)
    {
        close(wdata->notice_pipe[0]);
        wdata->notice_pipe[0] = -1;
    }

    if(wdata->notice_pipe[1] > 0)
    {
        close(wdata->notice_pipe[1]);
        wdata->notice_pipe[1] = -1;
    }
}

static int readThreadCron(struct aeEventLoop *eventLoop, long long id, void *clientData)
{
    read_thread_data *rdata = clientData;
    listIter *li;
    listNode *ln;
    redis_node *srnode;

    RMT_NOTUSED(eventLoop);
    RMT_NOTUSED(id);
    RMT_NOTUSED(clientData);

    log_debug(LOG_VERB, "writeThreadCron() %lld", id);

    /* Update the time */
    rdata->unixtime = rmt_msec_now();

    //Check error connection
    li = listGetIterator(rdata->nodes_data, AL_START_HEAD);
    while ((ln = listNext(li)) != NULL) {
        srnode = listNodeValue(ln);
        redisSlaveReplCorn(srnode);
    }
    listReleaseIterator(li);
    
    return 1000;
}

static int writeThreadCron(struct aeEventLoop *eventLoop, long long id, void *clientData)
{
    int ret;
    write_thread_data *wdata = clientData;
    redis_group *trgroup = wdata->trgroup;
    rmtContext *ctx = trgroup->ctx;
    dictIterator *di;
    dictEntry *de;
    redis_node *trnode;
    tcp_context *tc;

    RMT_NOTUSED(eventLoop);
    RMT_NOTUSED(id);
    RMT_NOTUSED(clientData);

    log_debug(LOG_VERB, "writeThreadCron() %lld", id);

    /* Update the time */
    wdata->unixtime = rmt_msec_now();

    //Check error connection
    di = dictGetSafeIterator(trgroup->nodes);
    while ((de = dictNext(di)) != NULL) {
        trnode = dictGetVal(de);
        tc = trnode->tc;
        if (tc->sd <= 0 && listLength(trnode->send_data) > 0) {
            ret = rmt_tcp_context_reconnect(tc);
            if (ret != RMT_OK) {
                log_error("ERROR: reconnect to %s failed", trnode->addr);
                continue;
            }

            if (ctx->noreply == 0) {
                ret = aeCreateFileEvent(wdata->loop, tc->sd, 
                    AE_READABLE, recv_data_from_target, trnode);
                if (ret != AE_OK) {
                    log_error("ERROR: send_data event create %ld failed: %s",
                        wdata->thread_id, strerror(errno));
                    continue;
                }

                log_debug(LOG_NOTICE, "node[%s] create read event for thread %ld", 
                    trnode->addr, wdata->thread_id);
            }

            ret = aeCreateFileEvent(wdata->loop, tc->sd, 
                AE_WRITABLE, send_data_to_target, trnode);
            if(ret != AE_OK)
            {
                log_error("ERROR: send_data event create %ld failed: %s",
                    wdata->thread_id, strerror(errno));
                continue;
            }
        }
    }
    dictReleaseIterator(di);
    
    return 1000;
}

static void *event_run(void *args)
{
    aeMain(args);
    return 0;
}

static int assign_threads(int node_count, int thread_count,
    int *read_threads, int *write_threads)
{
    int factor; //used to assign scan thread number and delete thread number
    int remainder_threads;
    int read_threads_count, write_threads_count;

    if(node_count < 1 || thread_count < 1 ||
        read_threads == NULL ||
        write_threads == NULL)
    {
        return RMT_ERROR;
    }

    /*avoid read job too much faster 
        than write job and used too much memory,
        we set factor=20 to let read thread less 
        than write thread.
       */
    factor = 20;
    
	read_threads_count = (thread_count*factor)/100;
    if(read_threads_count <= 0)
    {
        read_threads_count = 1;
    }
	else if(read_threads_count > node_count)
	{
		read_threads_count = node_count;
	}
	
	write_threads_count = thread_count - read_threads_count;
	if(write_threads_count == 0)
	{
		read_threads_count --;
		write_threads_count ++;
	}
	else if(write_threads_count > node_count)
	{
		write_threads_count = node_count;
	}

    remainder_threads = thread_count - (read_threads_count + write_threads_count);
    while(remainder_threads > 0)
    {
        if(write_threads_count < node_count)
        {
            read_threads_count ++;
            remainder_threads --;
        }
        else if(read_threads_count < node_count)
        {
            read_threads_count ++;
            remainder_threads --;
        }
        else
        {
            break;
        }
    }

    log_notice("Nodes count of source group : %d", node_count);
    log_notice("Total threads count : %d", thread_count);
    log_notice("Read threads count assigned: %d", read_threads_count);
    log_notice("Write threads count assigned: %d", write_threads_count);

    *read_threads = read_threads_count;
    *write_threads = write_threads_count;

    return RMT_OK;
}

static int
instances_by_address_count_cmp(const void *t1, const void *t2)
{
    const list *ct1 = t1, *ct2 = t2;

    if (listLength(ct1) == listLength(ct2)) {
        return 0;
    } else if (listLength(ct1) > listLength(ct2)) {
        return -1;
    } else {
        return 1;
    }
}

static int read_write_threads_create(rmtContext *ctx, 
    dict *nodes, 
    int read_thread_count, 
    int write_thread_count,
    struct array *read_threads, 
    struct array *write_threads)
{
    int ret;
    uint32_t i, j;
    int found, average, read_finish, write_finish;
    int node_count;
    struct array instances_by_host;
    list *instances;
    read_thread_data *read_data, *read_data_min_rnodes;
    write_thread_data *write_data, *write_data_min_rnodes;
    redis_node *srnode, *rnode, *pre_node;
    dictIterator *di = NULL;
    dictEntry *de;
    listNode *lnode;

    
    if (nodes == NULL || 
        read_thread_count < 1 || write_thread_count < 1 ||
        read_threads == NULL || write_threads == NULL) {
        return RMT_ERROR;
    }

    read_finish = write_finish = 0;
    node_count = (int)dictSize(nodes);
    if (node_count < 1) {
        return RMT_ERROR;
    }

    ret = array_init(&instances_by_host,10,sizeof(list));
    if (ret != RMT_OK) {
        return RMT_ENOMEM;
    }

    di = dictGetIterator(nodes);
    while ((de = dictNext(di)) != NULL) {
        found = 0;
        srnode = dictGetVal(de);
        for (i = 0; i < array_n(&instances_by_host); i ++) {
            instances = array_get(&instances_by_host, i);
            rnode = listFirstValue(instances);
            if (!memcmp(rnode->addr,srnode->addr,
                MIN(strchr(rnode->addr,':')-rnode->addr,
                strchr(srnode->addr,':')-srnode->addr))){
                found = 1;
                break;
            }
        }

        if (found) {
            listAddNodeTail(instances,srnode);
        } else {
            instances = array_push(&instances_by_host);
            listInit(instances);
            listAddNodeTail(instances,srnode);
        }
    }
    dictReleaseIterator(di);

    array_sort(&instances_by_host, instances_by_address_count_cmp);

    if (array_n(&instances_by_host) <= read_thread_count) {
        ret = array_init(read_threads,array_n(&instances_by_host),sizeof(read_thread_data));
        if (ret != RMT_OK) {
            goto error;
        }

        for (i = 0; i < array_n(&instances_by_host); i ++) {
            instances = array_get(&instances_by_host, i);
            read_data = array_push(read_threads);
    		ret = read_thread_data_init(read_data);
    		if (ret != RMT_OK) {
    			goto error;
    		}

            read_data->id = i;

            read_data->nodes_count = listLength(instances);
            lnode = instances->head;
            while (lnode) {
                rnode = lnode->value;
                lnode = lnode->next;
                listAddNodeTail(read_data->nodes_data, rnode);
                rnode->read_data = read_data;
            }
        }

        read_finish = 1;
    } else {
        ret = array_init(read_threads,read_thread_count,sizeof(read_thread_data));
        if (ret != RMT_OK) {
            goto error;
        }
    }

    if (array_n(&instances_by_host) <= write_thread_count) {
        ret = array_init(write_threads,array_n(&instances_by_host),sizeof(write_thread_data));
        if (ret != RMT_OK) {
            goto error;
        }

        for (i = 0; i < array_n(&instances_by_host); i ++) {
            instances = array_get(&instances_by_host, i);
            write_data = array_push(write_threads);
    		ret = write_thread_data_init(ctx, write_data);
    		if (ret != RMT_OK) {
    			goto error;
    		}

            write_data->id = i;

            write_data->nodes_count = listLength(instances);
            pre_node = NULL;
            lnode = instances->head;
            while (lnode) {
                rnode = lnode->value;
                lnode = lnode->next;
                listAddNodeTail(write_data->nodes, rnode);
                rnode->write_data = write_data;
                
                if (pre_node) {
                    ASSERT(pre_node->next == NULL);
                    pre_node->next = rnode;
                }

                pre_node = rnode;

                ret = aeCreateFileEvent(write_data->loop, rnode->notice_pipe[0], 
                    AE_READABLE, parse_prepare, rnode);
                if(ret != AE_OK)
                {
                    log_error("ERROR: Create readable notice event for node[%s] fd %d "
                        "on the write thread %ld failed: %s",
                        rnode->addr, rnode->notice_pipe[0],
                        write_data->thread_id, strerror(errno));        
                    goto error;
                }
            }
        }

        write_finish = 1;
    } else {
        ret = array_init(write_threads,write_thread_count,sizeof(write_thread_data));
        if (ret != RMT_OK) {
            goto error;
        }
    }

    if (read_finish && write_finish) {
        goto done;
    }

    for (i = 0; i < array_n(&instances_by_host); i ++) {
        instances = array_get(&instances_by_host, i);

        if (!read_finish) {
            if (array_n(read_threads) < read_thread_count) {
                read_data = array_push(read_threads);
                ret = read_thread_data_init(read_data);
        		if (ret != RMT_OK) {
        			goto error;
        		}

                read_data->id = i;
                
                read_data->nodes_count = listLength(instances);
                lnode = instances->head;
                while (lnode) {
                    rnode = lnode->value;
                    lnode = lnode->next;
                    listAddNodeTail(read_data->nodes_data, rnode);
                    rnode->read_data = read_data;
                }
            } else {
                read_data_min_rnodes = NULL;
                for (j = 0; j < array_n(read_threads); j ++) {
                    read_data = array_get(read_threads, j);
                    if (read_data_min_rnodes == NULL) {
                        read_data_min_rnodes = read_data;
                        continue;
                    }

                    if(listLength(read_data_min_rnodes->nodes_data) > 
                        listLength(read_data->nodes_data)) {
                        read_data_min_rnodes = read_data;
                    }
                }

                read_data_min_rnodes->nodes_count += listLength(instances);
                lnode = instances->head;
                while (lnode) {
                    rnode = lnode->value;
                    lnode = lnode->next;
                    listAddNodeTail(read_data_min_rnodes->nodes_data, rnode);
                    rnode->read_data = read_data_min_rnodes;
                }
            }
        }

        if (!write_finish) {
            if (array_n(write_threads) < write_thread_count) {
                write_data = array_push(write_threads);
        		ret = write_thread_data_init(ctx, write_data);
        		if (ret != RMT_OK) {
        			goto error;
        		}

                write_data->id = i;

                write_data->nodes_count = listLength(instances);
                pre_node = NULL;
                lnode = instances->head;
                while (lnode) {
                    rnode = lnode->value;
                    lnode = lnode->next;
                    listAddNodeTail(write_data->nodes, rnode);
                    rnode->write_data = write_data;
                    
                    if (pre_node) {
                        ASSERT(pre_node->next == NULL);
                        pre_node->next = rnode;
                    }

                    pre_node = rnode;
                    ret = aeCreateFileEvent(write_data->loop, rnode->notice_pipe[0], 
                        AE_READABLE, parse_prepare, rnode);
                    if (ret != AE_OK) {
                        log_error("ERROR: Create readable notice event for node[%s] fd %d "
                            "on the write thread %ld failed: %s",
                            rnode->addr, rnode->notice_pipe[0],
                            write_data->thread_id, strerror(errno));        
                        goto error;
                    }
                }
            } else {
                write_data_min_rnodes = NULL;
                for (j = 0; j < array_n(write_threads); j ++) {
                    write_data = array_get(write_threads, j);
                    if (write_data_min_rnodes == NULL) {
                        write_data_min_rnodes = write_data;
                        continue;
                    }

                    if (listLength(write_data_min_rnodes->nodes) > 
                        listLength(write_data->nodes)) {
                        write_data_min_rnodes = write_data;
                    }
                }

                write_data_min_rnodes->nodes_count += listLength(instances);
                pre_node = listLastValue(write_data_min_rnodes->nodes);
                lnode = instances->head;
                while (lnode) {
                    rnode = lnode->value;
                    lnode = lnode->next;
                    listAddNodeTail(write_data_min_rnodes->nodes, rnode);
                    rnode->write_data = write_data_min_rnodes;

                    if (pre_node) {
                        ASSERT(pre_node->next == NULL);
                        pre_node->next = rnode;
                    }

                    pre_node = rnode;
                    ret = aeCreateFileEvent(write_data_min_rnodes->loop, 
                        rnode->notice_pipe[0], 
                        AE_READABLE, parse_prepare, rnode);
                    if (ret != AE_OK) {
                        log_error("ERROR: Create readable notice event for node[%s] fd %d "
                            "on the write thread %ld failed: %s",
                            rnode->addr, rnode->notice_pipe[0],
                            write_data_min_rnodes->thread_id, 
                            strerror(errno));        
                        goto error;
                    }
                }
            }
        }
    }

done:
    
    log_notice("instances_by_host:");
    for (i = 0; i < array_n(&instances_by_host); i ++) {
        instances = array_get(&instances_by_host, i);
        while (rnode = listPop(instances)) {
            log_notice("%s", rnode->addr);
        }
        log_notice("");
    }
    instances_by_host.nelem = 0;
    array_deinit(&instances_by_host);

    return RMT_OK;

error:

    for (i = 0; i < array_n(&instances_by_host); i ++) {
        instances = array_get(&instances_by_host, i);
        while (listPop(instances)) {}
    }
    instances_by_host.nelem = 0;
    array_deinit(&instances_by_host);

    return RMT_ERROR;
}

static void read_threads_destroy(struct array *read_datas);

static struct array *read_threads_create_unsafe(int read_threads_count, 
    int node_count, redis_group *srgroup)
{
    int ret;
    int i, k;
    int modulo, remainders, add_one_count;
    int begin, step;
    struct array *read_datas = NULL; //type : read_thread_data
    read_thread_data *read_data;
    dict *srnodes = srgroup->nodes;
    redis_node *srnode;
    dictIterator *di = NULL;
    dictEntry *de;

    if(srnodes == NULL || read_threads_count <= 0){
        return NULL;
    }

    ASSERT(node_count == (int)(dictSize(srnodes)));

    read_datas = array_create((uint32_t)read_threads_count, 
		sizeof(read_thread_data));
    if(read_datas == NULL)
    {
        log_stdout("create read thread data failed: out of memory");
        goto error;
    }

	modulo = node_count/read_threads_count;
	remainders = node_count%read_threads_count;
    add_one_count = remainders;
    begin = 0;
	step = modulo;
	if(add_one_count > 0)
	{
		step ++;
		add_one_count --;
	}
    
    di = dictGetIterator(srnodes);
	for(i = 0; i < read_threads_count; i ++)
	{
		read_data = array_push(read_datas);
		ret = read_thread_data_init(read_data);
		if(ret != RMT_OK)
		{
			goto error;
		}

        read_data->id = i;

        read_data->nodes_count = step;
		for(k = begin; k < begin + step; k ++)
		{   
		    de = dictNext(di);
            if(de == NULL){
                log_error("ERROR: Next node in the dict is NULL");
                goto error;
            }
            
            srnode = dictGetVal(de);
            if(srnode == NULL){
                log_error("ERROR: Source redis node in the dict is NULL");
                goto error;
            }
            
			listAddNodeTail(read_data->nodes_data, srnode);
            srnode->read_data = read_data;
		}

		begin += step;
		step = modulo;
		if(add_one_count > 0)
		{
			step ++;
			add_one_count --;
		}
	}
    dictReleaseIterator(di);
    
    return read_datas;

error:

    if(read_datas != NULL)
    {
        read_threads_destroy(read_datas);
    }

    if(di != NULL){
        dictReleaseIterator(di);
    }

    return NULL;
}

static void read_threads_destroy(struct array *read_datas)
{
    read_thread_data *read_data;

    if(read_datas == NULL)
    {
        return;
    }
    
    while(array_n(read_datas) > 0)
    {
        read_data = array_pop(read_datas);
        read_thread_data_deinit(read_data);
    }
    
    array_destroy(read_datas);
}

static struct array *write_threads_create_unsafe(rmtContext *ctx, int write_threads_count, 
    int node_count, redis_group *srgroup)
{
    int ret;
    int i, k;
    int modulo, remainders, add_one_count;
    int begin, step;
    struct array *write_datas = NULL; //type : write_thread_data
    write_thread_data *write_data;
    dict *srnodes = srgroup->nodes;
    redis_node *srnode, *pre_node = NULL;
    dictIterator *di = NULL;
    dictEntry *de;

    if(ctx == NULL || srnodes == NULL){
        return NULL;
    }

    ASSERT(node_count == (int)(dictSize(srnodes)));
    
    write_datas = array_create((uint32_t)write_threads_count, 
		sizeof(write_thread_data));
    if(write_datas == NULL)
    {
        log_error("ERROR: out of memory");
        goto error;
    }

	modulo = node_count/write_threads_count;
	remainders = node_count%write_threads_count;
    add_one_count = remainders;
	begin = 0;
	step = modulo;
	if(add_one_count > 0)
	{
		step ++;
		add_one_count --;
	}

    di = dictGetIterator(srnodes);
    
    for(i = 0; i < write_threads_count; i ++)
    {
    	write_data = array_push(write_datas);
    	ret = write_thread_data_init(ctx, write_data);
    	if(ret != RMT_OK)
    	{
    		goto error;
    	}

        write_data->id = i;
        
        write_data->nodes_count = step;
    	for(k = begin; k < begin + step; k ++)
    	{
    		de = dictNext(di);
            if(de == NULL){
                log_error("ERROR: Next node in the dict is NULL");
                goto error;
            }
            
            srnode = dictGetVal(de);
            if(srnode == NULL){
                log_error("ERROR: Source redis node in the dict is NULL");
                goto error;
            }

            listAddNodeTail(write_data->nodes, srnode);
            srnode->write_data = write_data;

            if(pre_node != NULL){
                ASSERT(pre_node->next == NULL);
                pre_node->next = srnode;
            }

            pre_node = srnode;

            ret = aeCreateFileEvent(write_data->loop, srnode->notice_pipe[0], 
                AE_READABLE, parse_prepare, srnode);
            if(ret != AE_OK)
            {
                log_error("ERROR: create readable notice event for node[%s] fd %d "
                    "on the write thread %ld failed: %s",
                    srnode->addr, srnode->notice_pipe[0],
                    write_data->thread_id, strerror(errno));        
                goto error;
            }
    	}

    	begin += step;
    	step = modulo;
    	if(add_one_count > 0)
    	{
    		step ++;
    		add_one_count --;
    	}
    }
    dictReleaseIterator(di);
    
    return write_datas;

error:

    if(write_datas != NULL)
    {
        read_threads_destroy(write_datas);
    }

    if(di != NULL){
        dictReleaseIterator(di);
    }

    return NULL;
}

static void write_threads_destroy(struct array *write_datas)
{
    write_thread_data *write_data;

    if(write_datas == NULL)
    {
        return;
    }
    
    while(array_n(write_datas) > 0)
    {
        write_data = array_pop(write_datas);
        write_thread_data_deinit(write_data);
    }
    
    array_destroy(write_datas);
}

static void *read_thread_run_old(void *args)
{
    read_thread_data *read_data = args;
    list *nodes_data = read_data->nodes_data;  //type : source redis_node
    redis_node *srnode;
    listNode *lnode;
    listIter *it;

    it = listGetIterator(nodes_data, AL_START_HEAD);
    while((lnode = listNext(it)) != NULL)
    {
    	srnode = listNodeValue(lnode);
        rmtConnectRedisMaster(srnode);
    }
    
    listReleaseIterator(it);

    aeMain(read_data->loop);

    return 0;
}

static void begin_replication(aeEventLoop *el, int fd, void *privdata, int mask)
{
    char c[1];
    redis_node *srnode = privdata;
    read_thread_data *read_data = srnode->read_data;
    
    RMT_NOTUSED(el);
    RMT_NOTUSED(fd);
    RMT_NOTUSED(privdata);
    RMT_NOTUSED(mask);

    ASSERT(read_data->loop == el);
    ASSERT(srnode->notice_read_pipe[0] == fd);

    rmt_read(srnode->notice_read_pipe[0], c , 1);
    ASSERT(c[0] == ' ');

    aeDeleteFileEvent(el, fd, mask);

    close(srnode->notice_read_pipe[0]);
    srnode->notice_read_pipe[0] = -1;
    close(srnode->notice_read_pipe[1]);
    srnode->notice_read_pipe[1] = -1;
    
    rmtConnectRedisMaster(srnode);
}

static void *read_thread_run(void *args)
{
    int ret;
    read_thread_data *read_data = args;
    list *nodes_data = read_data->nodes_data;  //type : source redis_node
    redis_node *srnode;
    listNode *lnode;
    listIter *it;

    it = listGetIterator(nodes_data, AL_START_HEAD);
    while((lnode = listNext(it)) != NULL){
    	srnode = listNodeValue(lnode);
        ret = aeCreateFileEvent(read_data->loop, srnode->notice_read_pipe[0], 
                AE_READABLE, begin_replication, srnode);
        if(ret != AE_OK)
        {
            log_error("ERROR: Create readable notice event for node[%s] fd %d "
                "to begin replication on the read thread %ld failed: %s",
                srnode->addr, srnode->notice_read_pipe[0],
                read_data->thread_id, strerror(errno));
            listReleaseIterator(it);
            exit(0);
        }
    }
    
    listReleaseIterator(it);

    aeMain(read_data->loop);

    return 0;
}

static void *write_thread_run(void *args)
{
    int ret;
    write_thread_data *write_data = args;
    list *nodes = write_data->nodes;  //type : source redis_node
    redis_node *srnode;
    redis_group *srgroup;

    srnode = listFirstValue(nodes);
    if(srnode == NULL){
        log_error("ERROR: No redis nodes for this write thread %ld", 
            write_data->thread_id);
        return 0;
    }

    srgroup = srnode->owner;
    if(srgroup->kind == GROUP_TYPE_RDBFILE){
        listNode *lnode;
        listIter *it;
        it = listGetIterator(nodes, AL_START_HEAD);
        while((lnode = listNext(it)) != NULL)
        {
        	srnode = listNodeValue(lnode);
            
            if(srnode->sk_event < 0){
                srnode->sk_event = socket(AF_INET, SOCK_STREAM, 0);
                if(srnode->sk_event < 0){
                    log_error("ERROR: Create sk_event for node[%s] failed: %s", 
                        srnode->addr, strerror(errno));
                    return 0;
                }
            }
            
            srnode->timestamp = rmt_msec_now();
            ret = aeCreateFileEvent(write_data->loop, srnode->sk_event, 
                AE_WRITABLE, redis_parse_rdb, srnode);
            if(ret == AE_ERR)
            {
                log_error("ERROR: Create ae write event for node %s parse rdb file failed", 
                    srnode->addr);
                listReleaseIterator(it);
                return 0;
            }
        }
        
        listReleaseIterator(it);
        
        aeMain(write_data->loop);
        return 0;
    }

    rmt_write(srnode->notice_read_pipe[1], " ", 1);

    aeMain(write_data->loop);

    return 0;
}

static void target_node_close(redis_node *trnode)
{
    tcp_context *tc = trnode->tc;
    struct msg *msg;

    ASSERT(trnode->sent_data != NULL);

    rmt_tcp_context_close_sd(tc);

    while ((msg = listPop(trnode->sent_data)) != NULL) {
        ASSERT(msg->request && msg->sent);
        msg_put(msg);
        msg_free(msg);
    }
    
    if (trnode->msg_rcv != NULL) {
        msg_put(trnode->msg_rcv);
        msg_free(trnode->msg_rcv);
        trnode->msg_rcv = NULL;
    }
}

static void send_data_to_target(aeEventLoop *el, int fd, void *privdata, int mask)
{
    int ret;
    redis_node *trnode = privdata;
    tcp_context *tc = trnode->tc;
    write_thread_data *wdata = trnode->write_data;
    listNode *lnode_node, *lnode_msg, *lnode_mbuf;
    list send_msgl;                      /* send msg list */
    struct iovec *ciov, iov[RMT_IOV_MAX];
    struct msg *msg;
    struct mbuf *mbuf;                   /* current mbuf */
    size_t mlen;                         /* current mbuf data length */
    struct array sendv;                  /* send iovec */
    size_t limit;                        /* bytes to send limit */
    size_t nsend, nsent;                 /* bytes to send; bytes sent */
    ssize_t n;                           /* bytes sent by sendv */
    int stop;
    int send_again;

    RMT_NOTUSED(el);
    RMT_NOTUSED(fd);
    RMT_NOTUSED(privdata);
    RMT_NOTUSED(mask);

    ASSERT(fd == tc->sd);

again:

    send_again = 1;
    nsend = 0;
    stop = 0;
    limit = SSIZE_MAX;
    
    listInit(&send_msgl);
    array_set(&sendv, iov, sizeof(iov[0]), RMT_IOV_MAX);

    log_debug(LOG_DEBUG, "send_data_to_target node[%s] msgs %lld",
        trnode->addr, listLength(trnode->send_data));
    
    lnode_msg = listFirst(trnode->send_data);
    
    while(lnode_msg != NULL && !stop){
        
        msg = listNodeValue(lnode_msg);
        ASSERT(msg != NULL);
        
        listAddNodeTail(&send_msgl, lnode_msg);

        lnode_mbuf = listFirst(msg->data);
        while(lnode_mbuf != NULL){

            if(array_n(&sendv) >= RMT_IOV_MAX || 
                nsend >= limit){
                stop = 1;
                break;
            }
            
            mbuf = listNodeValue(lnode_mbuf);

            if (mbuf_empty(mbuf)) {
                lnode_mbuf = listNextNode(lnode_mbuf);
                continue;
            }

            mlen = mbuf_length(mbuf);
            
            ciov = array_push(&sendv);
            ciov->iov_base = mbuf->pos;
            ciov->iov_len = mlen;

            nsend += mlen;
            
            lnode_mbuf = listNextNode(lnode_mbuf);
        }

        lnode_msg = listNextNode(lnode_msg);
    }

    log_debug(LOG_DEBUG, "%u mbufs %u bytes will be sent", 
        array_n(&sendv), nsend);

    if (listLength(&send_msgl) > 0 && nsend != 0) {
        n = rmt_sendv(fd, &sendv, nsend);
        if(n == RMT_ERROR){
            log_error("ERROR: errors on connection with node[%s]", trnode->addr);

            //disconnect it and it will reconnect at the writeThreadCron
            aeDeleteFileEvent(el, tc->sd, AE_READABLE|AE_WRITABLE);
            target_node_close(trnode);
        }

        if(n < (ssize_t)nsend){
            send_again = 0;
        }
    }else{
        n = 0;
    }

    nsent = n > 0 ? (size_t)n : 0;

    wdata->stat_total_net_output_bytes += nsent;

    log_debug(LOG_DEBUG, "%u bytes has be sent", nsent);

    while((lnode_node = listFirst(&send_msgl)) != NULL){
        lnode_msg = listNodeValue(lnode_node);
        msg = listNodeValue(lnode_msg);
        listDelNode(&send_msgl, lnode_node);

        if (nsent == 0) {
            if (msg->mlen == 0) {
                //msg send done?
                log_debug(LOG_NOTICE, "");
                MSG_DUMP(msg,LOG_NOTICE);
                NOT_REACHED();
            }
            
            continue;
        }

        /* adjust mbufs of the sent message */
        lnode_mbuf = listFirst(msg->data);
        while(lnode_mbuf != NULL){
            
            mbuf = listNodeValue(lnode_mbuf);

            if (mbuf_empty(mbuf)) {
                lnode_mbuf = listNextNode(lnode_mbuf);
                continue;
            }

            mlen = mbuf_length(mbuf);
            if (nsent < mlen) {
                /* mbuf was sent partially; process remaining bytes later */
                mbuf->pos += nsent;
                ASSERT(mbuf->pos < mbuf->last);
                nsent = 0;
                break;
            }

            /* mbuf was sent completely; mark it empty */
            mbuf->pos = mbuf->last;
            nsent -= mlen;
            
            lnode_mbuf = listNextNode(lnode_mbuf);
        }

        /* message has been sent completely, finalize it */
        if(lnode_mbuf == NULL){
            //msg send done
            ASSERT(listFirst(trnode->send_data) == lnode_msg);
            listDelNode(trnode->send_data, lnode_msg);
            if(msg->noreply){
                ASSERT(listLength(trnode->sent_data) == 0);
                wdata->stat_msgs_outqueue --;
                msg_put(msg);
                msg_free(msg);
                wdata->stat_total_msgs_sent ++;
            }else{
                msg->sent = 1;
                listAddNodeTail(trnode->sent_data,msg);
            }
        }
    }

    ASSERT(listLength(&send_msgl) == 0);

    if(listLength(trnode->send_data) == 0){
        aeDeleteFileEvent(el, fd, AE_WRITABLE);
    }else if(send_again == 1){
        goto again;
    }
}

static void recv_data_from_target(aeEventLoop *el, int fd, void *privdata, int mask)
{
    int ret;
    redis_node *trnode = privdata;
    redis_group *trgroup = trnode->owner;
    mbuf_base *mb = trgroup->mb;
    struct msg *msg;
    struct mbuf *mbuf;
    ssize_t nread;
    size_t msize;
    
    RMT_NOTUSED(el);
    RMT_NOTUSED(fd);
    RMT_NOTUSED(privdata);
    RMT_NOTUSED(mask);

again:
    
    if(trnode->msg_rcv == NULL){
        trnode->msg_rcv = msg_get(mb, 0, 0);
    }

    msg = trnode->msg_rcv;
    if(msg == NULL){
        log_error("ERROR: out of memory");
        return;
    }

    mbuf = listLastValue(msg->data);
    if(mbuf == NULL || mbuf_full(mbuf)){
        mbuf = mbuf_get(mb);
        if(mbuf == NULL){
            log_error("ERROR: mbuf_get NULL");
            return;
        }
        
        listAddNodeTail(msg->data, mbuf);
        msg->pos = mbuf->pos;
    }

    msize = mbuf_size(mbuf);
    ASSERT(msize > 0);

    nread = rmt_read(fd, mbuf->last, msize);
    if (nread < 0) {
        if (errno == EINTR) {
            log_debug(LOG_VERB, "I/O no ready-eintr to read from target server[%s]: %s", 
                trnode->addr, strerror(errno));
            goto again;
        } else if(errno == EAGAIN || errno == EWOULDBLOCK){
            log_debug(LOG_VERB, "I/O no ready-eagain to read from target server[%s]: %s",
                trnode->addr, strerror(errno));
            nread = 0;
        }else{
            log_warn("I/O error read from target server[%s]: %s",
                trnode->addr, strerror(errno));
            aeDeleteFileEvent(el, fd, AE_READABLE);
        }

        return;
    }else if(nread == 0){
        log_warn("I/O error read from target server[%s]: lost connect",
            trnode->addr);
        aeDeleteFileEvent(el, fd, AE_READABLE);
        return;
    }
    
    ASSERT((mbuf->last + nread) <= mbuf->end);

    if(nread == 0){
        return;
    }

    mbuf->last += nread;
    msg->mlen += (uint32_t)nread;

    ret = parse_response(trnode);
    if(ret != RMT_OK)
    {
        log_error("ERROR: response msg parsed error");
        return;
    }

    if(nread < (ssize_t)msize){
        return;
    }

    goto again;
}

int prepare_send_msg(redis_node *srnode, struct msg *msg, redis_node *trnode)
{
    int ret;
    rmtContext *ctx = srnode->ctx;
    write_thread_data *wdata = srnode->write_data;
    redis_group *trgroup = wdata->trgroup;
    tcp_context *tc = trnode->tc;

    log_debug(LOG_DEBUG, "prepare_send_msg holds %u mbufs to node[%s]", 
        listLength(msg->data), trnode->addr);

    MSG_CHECK(ctx, msg);
    
    if (tc->sd < 0) {
        tc->flags &= ~RMT_BLOCK;    
        ret = rmt_tcp_context_connect_addr(tc, trnode->addr, 
            (int)rmt_strlen(trnode->addr), NULL, NULL);
        if (ret != RMT_OK) {
            log_error("ERROR: connect to %s failed", trnode->addr);
            return RMT_ERROR;
        }

        if (ctx->noreply == 0) {
            ret = aeCreateFileEvent(wdata->loop, tc->sd, 
                AE_READABLE, recv_data_from_target, trnode);
            if(ret != AE_OK){
                log_error("ERROR: send_data event create %ld failed: %s",
                    wdata->thread_id, strerror(errno));
                return RMT_ERROR;
            }

            log_debug(LOG_DEBUG, "node[%s] create read event for thread %ld", 
                trnode->addr, wdata->thread_id);
        }
    }
    
    ret = aeCreateFileEvent(wdata->loop, tc->sd, 
        AE_WRITABLE, send_data_to_target, trnode);
    if (ret != AE_OK) {
        log_error("ERROR: send_data event create %ld failed: %s",
            wdata->thread_id, strerror(errno));
        return RMT_ERROR;
    }

    listAddNodeTail(trnode->send_data, msg);
    trgroup->msg_send_num ++;
    wdata->stat_total_msgs_recv ++;
    wdata->stat_msgs_outqueue ++;
    log_debug(LOG_DEBUG, "sended msgs: %lld", 
        trgroup->msg_send_num);
    
    return RMT_OK;
}

static int prepare_send_data(redis_node *srnode)
{
    int ret;
    struct msg *msg, *sub_msg;
    struct keypos *kp;
    write_thread_data *write_data = srnode->write_data;
    rmtContext *ctx = srnode->ctx;
    redis_group *trgroup = write_data->trgroup;
    list frag_msgl;
    uint32_t slots;
    redis_node *trnode;

    listInit(&frag_msgl);

    ASSERT(trgroup != NULL && srnode->msg != NULL);
    
    msg = srnode->msg;
    srnode->msg = NULL;
    
    if(msg->noforward)
    {
        msg_put(msg);
        msg_free(msg);
        return RMT_OK;
    }

    slots = array_n(trgroup->route);
    ASSERT(slots > 0);

    MSG_CHECK(ctx, msg);

    //If this msg contain only one key, just send it.
    if (array_n(msg->keys) == 1) {
        kp = array_get(msg->keys, 0);
        trnode = trgroup->get_backend_node(trgroup, kp->start, (uint32_t)(kp->end-kp->start));
        if(prepare_send_msg(srnode, msg, trnode) != RMT_OK){
            goto error;
        }
        return RMT_OK;
    }

    ret = msg->fragment(trgroup, msg, slots, &frag_msgl);
    if (ret != RMT_OK) {
        log_error("ERROR: msg fragment failed");
        goto error;
    }

    if (listLength(&frag_msgl) == 0) {
        kp = array_get(msg->keys, 0);
        trnode = trgroup->get_backend_node(trgroup, kp->start, (uint32_t)(kp->end-kp->start));
        if(prepare_send_msg(srnode, msg, trnode) != RMT_OK){
            goto error;
        }
        return RMT_OK;
    }

    if (msg->frag_seq != NULL) {
        rmt_free(msg->frag_seq);
        msg->frag_seq = NULL;
    }

    while ((sub_msg = listPop(&frag_msgl)) != NULL) {
        kp = array_get(sub_msg->keys, 0);
        trnode = trgroup->get_backend_node(trgroup, kp->start, (uint32_t)(kp->end-kp->start));
        if(prepare_send_msg(srnode, sub_msg, trnode) != RMT_OK){
            msg_put(sub_msg);
            msg_free(sub_msg);
            goto error;
        }
    }

    msg_put(msg);
    msg_free(msg);
    
    return RMT_OK;

error:

    msg_put(msg);
    msg_free(msg);

    while ((sub_msg = listPop(&frag_msgl)) != NULL) {
        msg_put(sub_msg);
        msg_free(sub_msg);
    }
    
    return RMT_ERROR;
}

static int response_done(redis_node *trnode, struct msg *resp)
{
    int ret;
    struct msg *req;
    write_thread_data *wdata = trnode->write_data;
    
    if(trnode == NULL || resp == NULL){
        return RMT_ERROR;
    }

    MSG_DUMP(resp, LOG_DEBUG);

    ASSERT(trnode->msg_rcv == resp);
    
    req = listPop(trnode->sent_data);    
    wdata->stat_total_msgs_sent ++;
    wdata->stat_msgs_outqueue --;
    ASSERT(req != NULL);
    ASSERT(req->sent == 1);
    ASSERT(req->peer == NULL);
    req->peer = resp;

    log_debug(LOG_DEBUG, "%d msgs wait for response from target group", listLength(trnode->sent_data));
    
    trnode->msg_rcv = NULL;

    ret = req->resp_check(req);
    if(ret != RMT_OK){
        log_error("ERROR: Response check is error");
        return RMT_ERROR;
    }

    return RMT_OK;
}

void parse_prepare(aeEventLoop *el, int fd, void *privdata, int mask)
{
    int ret;
    redis_node *srnode = privdata;
    write_thread_data *write_data = srnode->write_data;
    redis_rdb *rdb = srnode->rdb;
    
    RMT_NOTUSED(el);
    RMT_NOTUSED(fd);
    RMT_NOTUSED(privdata);
    RMT_NOTUSED(mask);

    ASSERT(write_data->loop == el);
    ASSERT(srnode->notice_pipe[0] == fd);

    if (rdb->type == REDIS_RDB_TYPE_FILE) {
        aeDeleteFileEvent(write_data->loop, srnode->notice_pipe[0], AE_READABLE);
        
        if (srnode->sk_event < 0) {
            srnode->sk_event = socket(AF_INET, SOCK_STREAM, 0);
            if(srnode->sk_event < 0){
                log_error("ERROR: Create sk_event for node[%s] failed: %s", 
                    srnode->addr, strerror(errno));
                return;
            }
        }

        srnode->timestamp = rmt_msec_now();
        ret = aeCreateFileEvent(write_data->loop, srnode->sk_event, 
            AE_WRITABLE, redis_parse_rdb, srnode);
        if (ret == AE_ERR) {
            log_error("ERROR: Create ae write event for node %s parse_request failed", 
                srnode->addr);
            return;
        }
        
        return;     
    }

    aeDeleteFileEvent(write_data->loop, srnode->notice_pipe[0], AE_READABLE);
    
    ret = aeCreateFileEvent(write_data->loop, srnode->notice_pipe[0], 
        AE_READABLE, parse_request, srnode);
    if (ret != AE_OK) {
        log_error("ERROR: Create ae read event for node %s parse_request failed", 
            srnode->addr);
        return;
    }
    
    notice_write_thread(srnode);
}

void parse_request(aeEventLoop *el, int fd, void *privdata, int mask)
{
    int ret;
    char c[1];
    redis_node *srnode = privdata;
    rmtContext *ctx = srnode->ctx;
    redis_repl *rr = srnode->rr;
    redis_rdb *rdb = srnode->rdb;
    redis_group *srgroup = srnode->owner;
    write_thread_data *write_data = srnode->write_data;
    redis_group *trgroup = write_data->trgroup;
    mttlist *data;
    struct msg *msg;
    struct mbuf *mbuf_f, *mbuf_t;
    struct mbuf *mbuf, *nbuf;
    uint32_t len;
    int data_type;

    RMT_NOTUSED(el);
    RMT_NOTUSED(fd);
    RMT_NOTUSED(privdata);
    RMT_NOTUSED(mask);

    ASSERT(el == write_data->loop);
    ASSERT(fd == srnode->notice_pipe[0]);

    log_debug(LOG_DEBUG, "parse_job %s", srnode->addr);

    if(rr->repl_state == REDIS_REPL_TRANSFER){
        data = rdb->data;
        data_type = REDIS_DATA_TYPE_RDB;
    }else if(rr->repl_state == REDIS_REPL_CONNECTED){
        if(!mttlist_empty(rdb->data)){
            data = rdb->data;
            data_type = REDIS_DATA_TYPE_RDB;
        }else{
            data = srnode->cmd_data;
            data_type = REDIS_DATA_TYPE_CMD;
        }
    }else{
        log_error("ERROR: recieve data node state is error: %d", rr->repl_state);
        return;
    }
    
    while(1){
        if(listLength(srnode->piece_data) > 0){
            log_debug(LOG_VVERB,"trgroup->piece_data:");
            mbuf_list_dump(srnode->piece_data, LOG_VVERB);
            mbuf_f = mbuf_list_pop(srnode->piece_data);
        }else{
            if(rmt_read(fd,c,1) < 1){
                //log_warn("read from notice sd failed");
            }
            
            mbuf_f = mttlist_pop(data);
        }

        log_debug(LOG_DEBUG, "srnode data(type %d) len %d", 
            data_type, mttlist_length(data));

        if(mbuf_f == NULL){
            log_debug(LOG_DEBUG, "No more data to parse, wait for receive data.");
            break;
        }
    
        if(srnode->msg == NULL){
            srnode->msg = msg_get(srgroup->mb, 1, data_type);
            if(srnode->msg != NULL){
                if(ctx->noreply){
                    srnode->msg->noreply = 1;
                }else{
                    srnode->msg->noreply = 0;
                }
            }
        }

        msg = srnode->msg;
        if(msg == NULL){
            log_error("ERROR: Out of memory");
            return;
        }

        if(msg->result == MSG_PARSE_REPAIR){
            mbuf_t = mbuf_f;

            if(listLength(srnode->piece_data) > 0){
                mbuf_f = mbuf_list_pop(srnode->piece_data);
            }else{
                if(rmt_read(fd,c,1) < 1){
                    //log_warn("read from notice sd failed");
                }
                
                mbuf_f = mttlist_pop(data);
            }

            if(mbuf_f == NULL){
                log_debug(LOG_DEBUG,"No more data to parse, wait for receive data.");
                ASSERT(listLength(srnode->piece_data) == 0);
                mbuf_list_push_head(srnode->piece_data, mbuf_t);
                break;
            }

            ASSERT(mbuf_t != NULL && 
                mbuf_storage_length(mbuf_t) > 0);

            len = MIN(mbuf_size(mbuf_t), mbuf_length(mbuf_f));

            ret = mbuf_move(mbuf_f, mbuf_t, len);
            if(ret != RMT_OK){
                log_error("ERROR: mbuf_f(%u) move data(%u) to mbuf_t(%u) failed",
                    mbuf_length(mbuf_f), len, mbuf_size(mbuf_t));
                return;
            }

            if(mbuf_empty(mbuf_f)){
                mbuf_put(mbuf_f);
            }else{
                mbuf_list_push_head(srnode->piece_data, mbuf_f);
                
                log_hexdump(LOG_VVERB, mbuf_f->pos, mbuf_length(mbuf_f), 
                    "mbuf_f input mbuf_list");
            }

            listAddNodeTail(msg->data, mbuf_t);
            msg->pos = mbuf_t->pos;
            msg->mlen += mbuf_length(mbuf_t);
        }else{
            listAddNodeTail(msg->data, mbuf_f);
            msg->pos = mbuf_f->pos;
            msg->mlen += mbuf_length(mbuf_f);
        }

        MSG_DUMP(msg, LOG_VVERB);
        msg->parser(msg);

        if(msg->result == MSG_PARSE_OK){
            log_debug(LOG_DEBUG, "msg %s parse ok", 
                msg_type_string(msg->type));

            mbuf = listLastValue(msg->data);
            if (msg->pos == mbuf->last){
                //send msg                
                prepare_send_data(srnode);
                continue;
            }

            ASSERT(msg->pos > mbuf->pos && msg->pos < mbuf->last);
            nbuf = msg_split(msg, msg->pos);
            if (nbuf == NULL) {
                log_error("ERROR: split msg failed: out of memory");
                return;
            }
    
            msg->mlen -= mbuf_length(nbuf);
            ASSERT(msg->mlen > 0);

            //send msg
            prepare_send_data(srnode);
        }else if(msg->result == MSG_PARSE_REPAIR){
            log_debug(LOG_DEBUG, "msg %s parse repair", 
                msg_type_string(msg->type));
            nbuf = msg_split(msg, msg->pos);
            if (nbuf == NULL) {
                log_error("ERROR: split msg failed: out of memory");
                return;
            }

            msg->mlen -= mbuf_length(nbuf);
        }else if(msg->result == MSG_PARSE_AGAIN){
            log_debug(LOG_DEBUG, "msg %s parse again", 
                msg_type_string(msg->type));
            continue;
        }else{
            MSG_DUMP_ALL(msg, LOG_NOTICE);
            mbuf_list_dump_all(srnode->piece_data, LOG_NOTICE);
            msg_put(srnode->msg);
            msg_free(srnode->msg);
            srnode->msg = NULL;

            //need to handle the error
            
            continue;
        }

        ASSERT(listLength(srnode->piece_data) <= 1);
        mbuf_list_push_head(srnode->piece_data, nbuf);
    }
}

int parse_response(redis_node *trnode)
{
    int ret;
    redis_group *trgroup = trnode->owner;
    mbuf_base *mb = trgroup->mb;
    struct msg *msg, *nmsg;
    struct mbuf *mbuf, *nbuf;
    
    if(trnode == NULL)
    {
        return RMT_ERROR;
    }

    msg = trnode->msg_rcv;
    if(msg == NULL)
    {
        log_error("ERROR: trnode->msg_rcv is null");
        return RMT_ERROR;
    }

    msg->parser(msg);
    
    if(msg->result == MSG_PARSE_OK){
        log_debug(LOG_DEBUG, "response msg parse ok");

        mbuf = listLastValue(msg->data);
        if (msg->pos == mbuf->last){
            //check response
            log_debug(LOG_DEBUG, "msg->pos == mbuf->last");
            ret = response_done(trnode, msg);
            if(ret != RMT_OK){
                log_error("ERROR: response done error");
            }
            
            return RMT_OK;
        }

        nbuf = msg_split(msg, msg->pos);
        if (nbuf == NULL) {
            log_error("ERROR: split msg failed: out of memory");
            return RMT_ERROR;
        }

        msg->mlen -= mbuf_length(nbuf);
        ASSERT(msg->mlen > 0);
        
        //check response
        ret = response_done(trnode, msg);
        if(ret != RMT_OK){
            log_error("ERROR: response done error");
        }

        nmsg = msg_get(mb, 0, 0);
        if(nmsg == NULL){
            log_error("ERROR: msg_get null");
            return RMT_ERROR;
        }

        listAddNodeTail(nmsg->data, nbuf);
        nmsg->pos = nbuf->pos;
        nmsg->mlen = mbuf_length(nbuf);

        trnode->msg_rcv = nmsg;

        return parse_response(trnode);
        //return RMT_OK;
    }else if(msg->result == MSG_PARSE_REPAIR){
        log_debug(LOG_DEBUG, "response msg parse repair");
        nbuf = msg_split(msg, msg->pos);
        if (nbuf == NULL) {
            log_error("ERROR: split msg failed: out of memory");
            return RMT_ERROR;
        }

        listAddNodeTail(msg->data, nbuf);
        msg->pos = nbuf->pos;
        
        return RMT_OK;
    }else if(msg->result == MSG_PARSE_AGAIN){
        log_debug(LOG_DEBUG, "response msg parse again");
        return RMT_OK;
    }else{
        log_debug(LOG_DEBUG, "response msg parse error");
        return RMT_ERROR;
    }

    return RMT_OK;
}

int notice_write_thread(redis_node *srnode)
{
    log_debug(LOG_DEBUG, "notice the write thread");
    
    if(srnode == NULL){
        return RMT_ERROR;
    }

    rmt_write(srnode->notice_pipe[1]," ",1);

    return RMT_OK;
}

static redis_group *
group_create_from_option(rmtContext *ctx, char *addrs, int type, int source)
{
    int ret;
    int i;
    sds *servers = NULL;
    int servers_count = 0;
    int node_count = 0;
    redis_group *rgroup = NULL;
    
    //init the source group
    rgroup = rmt_alloc(sizeof(*rgroup));
    if(rgroup == NULL){
        log_error("ERROR: Out of memory");
        goto error;
    }    

    if(type == GROUP_TYPE_RCLUSTER){
        ret = redis_cluster_init_from_addrs(rgroup, addrs);
        if(ret != RMT_OK){
            log_error("ERROR: Init redis cluster from option failed");
            goto error;
        }

        rgroup->source = source;
        rgroup->kind = type;
        
        return rgroup;
    }

    if(type != GROUP_TYPE_SINGLE){
        log_error("ERROR: Group type in the option must be single and redis cluster");
        goto error;
    }

    rgroup->kind = type;
    
    servers = sdssplitlen(addrs, (int)rmt_strlen(addrs),
        ADDRESS_SEPARATOR, rmt_strlen(ADDRESS_SEPARATOR), &servers_count);
    if(servers == NULL || servers_count <= 0)
    {
        log_error("ERROR: address parsed error");
        goto error;
    }

    node_count = servers_count;

    ret = redis_group_init(ctx, rgroup, NULL, source);
    if(ret != RMT_OK){
        log_error("ERROR: Init source group from option failed");
        goto error;
    }

    for(i = 0; i < servers_count; i ++){
        if(redis_group_add_node(rgroup, 
            servers[i], servers[i]) == NULL){
            log_error("ERROR: Redis group add node[%s] failed",
                servers[i]);
            goto error;
        }
    }
    
    rgroup->kind = type;
    
    sdsfreesplitres(servers, servers_count);

    return rgroup;

error:

    if(servers != NULL){
        sdsfreesplitres(servers, servers_count);
    }

    if(rgroup != NULL){
        redis_group_deinit(rgroup);
        rmt_free(rgroup);
    }

    return NULL;
}

redis_group *
source_group_create(rmtContext *ctx)
{
    int ret;
    rmt_conf *cf;
    conf_pool *cp;
    redis_group *srgroup = NULL;
    
    if(ctx == NULL){
        return NULL;
    }

    cf = ctx->cf;
    if(cf == NULL){
        return group_create_from_option(ctx, 
            ctx->source_addr, ctx->source_type, 1);
    }else{
        cp = &cf->source_pool;
        srgroup = rmt_alloc(sizeof(*srgroup));
        if(srgroup == NULL){
            log_error("ERROR: Out of memory");
            goto error;
        }

        ret = redis_group_init(ctx, srgroup, cp, 1);
        if(ret != RMT_OK){
            log_error("ERROR: Source redis group init from conf file failed");
            goto error;
        }
    }

    ctx->srgroup = srgroup;

    return srgroup;

error:

    if(srgroup != NULL){
        source_group_destroy(srgroup);
    }

    return NULL;
}

void
source_group_destroy(redis_group *srgroup)
{
    if(srgroup == NULL){
        return;
    }

    redis_group_deinit(srgroup);
    rmt_free(srgroup);
}

redis_group *
target_group_create(rmtContext *ctx)
{
    int ret;
    rmt_conf *cf;
    conf_pool *cp;
    redis_group *trgroup = NULL;
    
    if(ctx == NULL){
        return NULL;
    }

    cf = ctx->cf;
    if(cf == NULL){
        return group_create_from_option(ctx, 
            ctx->target_addr, ctx->target_type, 0);
    }else{
        cp = &cf->target_pool;
        trgroup = rmt_alloc(sizeof(*trgroup));
        if(trgroup == NULL){
            log_error("ERROR: Out of memory");
            goto error;
        }

        ret = redis_group_init(ctx, trgroup, cp, 0);
        if(ret != RMT_OK){
            log_error("ERROR: Target redis group init from conf file failed");
            goto error;
        }
    }

    return trgroup;

error:

    if(trgroup != NULL){
        target_group_destroy(trgroup);
    }

    return NULL;
}

void
target_group_destroy(redis_group *trgroup)
{
    if(trgroup == NULL){
        return;
    }

    redis_group_deinit(trgroup);
    rmt_free(trgroup);
}

void redis_migrate(rmtContext *ctx, int type)
{
    int ret;
    int i;
    int node_count = 0;
    int thread_count;
    int read_threads_count, write_threads_count;
    int threads_hold_nodes_count, node_next_nodes_count;
    struct array *nodes = NULL;    //type: redis_node
    redis_group *srgroup = NULL;
    struct array *read_datas = NULL; //type : read_thread_data
    read_thread_data *read_data;
    struct array *write_datas = NULL; //type : write_thread_data
    write_thread_data *write_data;
    redis_node *rnode;
    listNode *lnode;

    RMT_NOTUSED(type);

    if(ctx == NULL || ctx->source_addr == NULL){
        goto done;
    }

    signal(SIGPIPE, SIG_IGN);
    
    thread_count = ctx->thread_count;
    if(thread_count <= 0){
        log_error("ERROR: thread count <= 0");
        return;
    }else if(thread_count == 1){
        thread_count ++;
	}

    srgroup = source_group_create(ctx);
    if(srgroup == NULL){
        log_error("Error: Source redis group create failed");
        goto done;
    }

    node_count = (int)dictSize(srgroup->nodes);

    ret = assign_threads(node_count, thread_count, 
        &read_threads_count, &write_threads_count);
    if(ret != RMT_OK){
        log_error("Error: Assign threads failed");
        goto done;
    }

    //Create read and write threads data
    if(!ctx->source_safe || srgroup->kind == GROUP_TYPE_RDBFILE){        
        if(srgroup->kind == GROUP_TYPE_RDBFILE){
            read_threads_count = 0;
            write_threads_count = MIN(node_count,thread_count);
        }

        read_datas = read_threads_create_unsafe(read_threads_count, 
            node_count, srgroup);
        if(read_datas == NULL && read_threads_count > 0){
            log_error("Error: Read threads create failed");
            goto done;
        }

        write_datas = write_threads_create_unsafe(ctx, write_threads_count, 
            node_count, srgroup);
        if(write_datas == NULL){
            log_error("Error: Write threads create failed");
            goto done;
        }
    }else{
        read_datas = rmt_alloc(sizeof(struct array));
        if (read_datas == NULL) {
            log_error("Error: Out of memory");
            goto done;
        }

        array_null(read_datas);

        write_datas = rmt_alloc(sizeof(struct array));
        if (write_datas == NULL) {
            log_error("Error: Out of memory");
            goto done;
        }

        array_null(write_datas);
        
        ret = read_write_threads_create(ctx, srgroup->nodes, read_threads_count, 
            write_threads_count, read_datas, write_datas);
        if (ret != RMT_OK) {
            log_error("Error: assign threads failed.");
            goto done;
        }
    }

    read_threads_count = read_datas?array_n(read_datas):0;
    write_threads_count = write_datas?array_n(write_datas):0;

    log_notice("Total threads count in fact: %d", 
        read_threads_count+write_threads_count);
    log_notice("Read threads count in fact: %d", read_threads_count);
    log_notice("Write threads count in fact: %d", write_threads_count);

    //Check the read threads
    threads_hold_nodes_count = 0;
    for (i = 0; i < read_threads_count; i ++) {
        read_data = array_get(read_datas, i);
        threads_hold_nodes_count += read_data->nodes_count;
        
        log_notice("read thread(%d):", read_data->id);
        lnode = read_data->nodes_data->head;
        while (lnode) {
            rnode = lnode->value;
            lnode = lnode->next;

            log_notice("%s", rnode->addr);
            if (rnode->read_data != read_data) {
                log_error("Error: node %s read_data is not correct.", 
                    rnode->addr);
                goto done;
            }
        }

        if (listLength(read_data->nodes_data) != read_data->nodes_count) {
            log_error("Error: listLength(read_data->nodes_data) %d != read_data->nodes_count %d.", 
                    listLength(read_data->nodes_data), read_data->nodes_count);
            goto done;
        }
    }
    if (threads_hold_nodes_count != node_count && 
        srgroup->kind != GROUP_TYPE_RDBFILE) {
        log_error("Error: read threads hold node count %s is wrong", 
            threads_hold_nodes_count);
        goto done;
    }

    //Check the write threads
    threads_hold_nodes_count = 0;
    for (i = 0; i < write_threads_count; i ++) {
        write_data = array_get(write_datas, i);
        threads_hold_nodes_count += write_data->nodes_count;
        
        log_notice("write thread(%d):", write_data->id);
        lnode = write_data->nodes->head;
        while (lnode) {
            rnode = lnode->value;
            lnode = lnode->next;

            log_notice("%s", rnode->addr);
            if (rnode->write_data != write_data) {
                log_error("Error: node %s write_data is not correct.", 
                    rnode->addr);
                goto done;
            }
        }

        if (listLength(write_data->nodes) != write_data->nodes_count) {
            log_error("Error: listLength(write_data->nodes) %d != write_data->nodes_count %d.", 
                    listLength(write_data->nodes), write_data->nodes_count);
            goto done;
        }

        node_next_nodes_count = 0;
        rnode = listFirstValue(write_data->nodes);
        while (rnode) {
            node_next_nodes_count ++;
            rnode = rnode->next;
        }
        if (node_next_nodes_count != write_data->nodes_count) {
            log_error("Error: node_next_nodes_count %d != write_data->nodes_count %d.", 
                listLength(write_data->nodes), write_data->nodes_count);
            goto done;
        }
    }
    if (threads_hold_nodes_count != node_count) {
        log_error("Error: write threads hold node count %s is wrong", 
            threads_hold_nodes_count);
        goto done;
    }

    ctx->rdatas = read_datas;
    ctx->wdatas = write_datas;

    //Run the read job
    for(i = 0; i < read_threads_count; i ++){
    	read_data = array_get(read_datas, (uint32_t)i);

        pthread_create(&read_data->thread_id, 
        	NULL, read_thread_run, read_data);
    }
    
    //Run the write job
    for(i = 0; i < write_threads_count; i ++){
        write_data = array_get(write_datas, (uint32_t)i);

        pthread_create(&write_data->thread_id, 
            NULL, write_thread_run, write_data);
    }

    ret = proxy_begin(ctx);
    if (ret != RMT_OK) {
        goto done;
    }

    log_notice("migrate job is running...");

    aeMain(ctx->loop);

	//wait for the read job finish
	for(i = 0; i < read_threads_count; i ++){
		read_data = array_get(read_datas, (uint32_t)i);
		pthread_join(read_data->thread_id, NULL);
	}

	//wait for the write job finish
	for(i = 0; i < write_threads_count; i ++){
		write_data = array_get(write_datas, (uint32_t)i);
		pthread_join(write_data->thread_id, NULL);
	}

done:

    if (read_datas != NULL) {
        read_threads_destroy(read_datas);
    }

    if (write_datas != NULL) {
        write_threads_destroy(write_datas);
    }

    if (srgroup != NULL) {
        redis_group_deinit(srgroup);
        rmt_free(srgroup);
    }
}

void redis_compare(rmtContext *ctx, int type)
{
    int ret;
    int i;
    int node_count = 0;
    int thread_count;
    int read_threads_count, write_threads_count;
    int threads_hold_nodes_count;
    struct array *nodes = NULL;    //type: redis_node
    redis_group *srgroup = NULL;
    struct array *read_datas = NULL; //type : read_thread_data
    read_thread_data *read_data;
    struct array *write_datas = NULL; //type : write_thread_data
    write_thread_data *write_data;
    redis_node *rnode;
    listNode *lnode;

    RMT_NOTUSED(type);

    if (ctx == NULL || ctx->source_addr == NULL) {
        goto done;
    }

    signal(SIGPIPE, SIG_IGN);
    
    thread_count = ctx->thread_count;
    if (thread_count <= 0) {
        log_error("ERROR: thread count <= 0");
        return;
    } else if(thread_count == 1) {
        thread_count ++;
	}

    srgroup = source_group_create(ctx);
    if(srgroup == NULL){
        log_error("Error: Source redis group create failed");
        goto done;
    }

    node_count = (int)dictSize(srgroup->nodes);

    ret = assign_threads(node_count, thread_count, 
        &read_threads_count, &write_threads_count);
    if(ret != RMT_OK){
        log_error("Error: Assign threads failed");
        goto done;
    }

    //Create read and write threads data
    if(!ctx->source_safe || srgroup->kind == GROUP_TYPE_RDBFILE){        
        if(srgroup->kind == GROUP_TYPE_RDBFILE){
            read_threads_count = 0;
            write_threads_count = MIN(node_count,thread_count);
        }

        read_datas = read_threads_create_unsafe(read_threads_count, 
            node_count, srgroup);
        if(read_datas == NULL && read_threads_count > 0){
            log_error("Error: Read threads create failed");
            goto done;
        }

        write_datas = write_threads_create_unsafe(ctx, write_threads_count, 
            node_count, srgroup);
        if(write_datas == NULL){
            log_error("Error: Write threads create failed");
            goto done;
        }
    }else{
        read_datas = rmt_alloc(sizeof(struct array));
        if (read_datas == NULL) {
            log_error("Error: Out of memory");
            goto done;
        }

        array_null(read_datas);

        write_datas = rmt_alloc(sizeof(struct array));
        if (write_datas == NULL) {
            log_error("Error: Out of memory");
            goto done;
        }

        array_null(write_datas);
        
        ret = read_write_threads_create(ctx, srgroup->nodes, read_threads_count, 
            write_threads_count, read_datas, write_datas);
        if (ret != RMT_OK) {
            log_error("Error: assign threads failed.");
            goto done;
        }
    }

    read_threads_count = read_datas?array_n(read_datas):0;
    write_threads_count = write_datas?array_n(write_datas):0;

    log_notice("Total threads count in fact: %d", 
        read_threads_count+write_threads_count);
    log_notice("Read threads count in fact: %d", read_threads_count);
    log_notice("Write threads count in fact: %d", write_threads_count);

    //Check the read threads
    threads_hold_nodes_count = 0;
    for (i = 0; i < read_threads_count; i ++) {
        read_data = array_get(read_datas, i);
        threads_hold_nodes_count += read_data->nodes_count;
        
        log_notice("read thread %d:", i);
        lnode = read_data->nodes_data->head;
        while (lnode) {
            rnode = lnode->value;
            lnode = lnode->next;

            log_notice("%s", rnode->addr);
            if (rnode->read_data != read_data) {
                log_error("Error: node %s read_data is not correct.", 
                    rnode->addr);
                goto done;
            }
        }
    }
    if (threads_hold_nodes_count != node_count && 
        srgroup->kind != GROUP_TYPE_RDBFILE) {
        log_error("Error: read threads hold node count %s is wrong", 
            threads_hold_nodes_count);
        goto done;
    }

    //Check the write threads
    threads_hold_nodes_count = 0;
    for (i = 0; i < write_threads_count; i ++) {
        write_data = array_get(write_datas, i);
        threads_hold_nodes_count += write_data->nodes_count;
        
        log_notice("write thread %d:", i);
        lnode = write_data->nodes->head;
        while (lnode) {
            rnode = lnode->value;
            lnode = lnode->next;

            log_notice("%s", rnode->addr);
            if (rnode->write_data != write_data) {
                log_error("Error: node %s write_data is not correct.", 
                    rnode->addr);
                goto done;
            }
        }
    }
    if (threads_hold_nodes_count != node_count) {
        log_error("Error: write threads hold node count %s is wrong", 
            threads_hold_nodes_count);
        goto done;
    }

    //Run the read job
    for(i = 0; i < read_threads_count; i ++){
    	read_data = array_get(read_datas, (uint32_t)i);

        pthread_create(&read_data->thread_id, 
        	NULL, read_thread_run, read_data);
    }
    
    //Run the write job
    for(i = 0; i < write_threads_count; i ++){
        write_data = array_get(write_datas, (uint32_t)i);

        pthread_create(&write_data->thread_id, 
            NULL, write_thread_run, write_data);
    }

    log_notice("migrate job is running...");

	//wait for the read job finish
	for(i = 0; i < read_threads_count; i ++){
		read_data = array_get(read_datas, (uint32_t)i);
		pthread_join(read_data->thread_id, NULL);
	}

	//wait for the write job finish
	for(i = 0; i < write_threads_count; i ++){
		write_data = array_get(write_datas, (uint32_t)i);
		pthread_join(write_data->thread_id, NULL);
	}

done:

    if(read_datas != NULL){
        read_threads_destroy(read_datas);
    }

    if(write_datas != NULL){
        write_threads_destroy(write_datas);
    }

    if(srgroup != NULL){
        redis_group_deinit(srgroup);
        rmt_free(srgroup);
    }
}

static int do_command_in_group(redis_group *rgroup, int type)
{
    RMT_NOTUSED(rgroup);
    RMT_NOTUSED(type);
    
    return RMT_OK;
}

void group_state(rmtContext *ctx, int type)
{
    redis_group *rgroup;

    if(array_n(&ctx->args) != 1){
        log_error("ERROR: Command %s must have one argument" 
            " that is 'source' or 'target'.",
            ctx->cmd);
        return;
    }

    if(strcmp(array_get(&ctx->args, 0), "source") == 0){
        rgroup = source_group_create(ctx);
    }else if(strcmp(array_get(&ctx->args, 0), "target") == 0){
        rgroup = target_group_create(ctx);
    }else{
        log_error("ERROR: Command %s must have one argument" 
            " that is 'source' or 'target'.",
            ctx->cmd);
        return;
    }

    if(rgroup == NULL){
        log_error("ERROR: Group create failed");
        return;
    }

    do_command_in_group(rgroup, type);

    redis_group_deinit(rgroup);
    rmt_free(rgroup);
}

unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, (int)sdslen((char*)key));
}

int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    size_t l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return strncasecmp(key1, key2, l1) == 0;
}

void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

void dictGroupNodeDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    redis_node_deinit(val);

    rmt_free(val);
}

int core_core(rmtContext *ctx)
{
    dictEntry *di;
    RMTCommand *command;
    int args_num;

#ifdef RMT_MEMORY_TEST
    mbuf_used_init();
    msg_used_init();
#endif
    
    //struct timeval timeout = { 3, 5000 };//3.005s

    di = dictFind(ctx->commands, ctx->cmd);
    if(di == NULL){
        log_stdout("ERR: command [%s] not found, please read the help.", ctx->cmd);
        return RMT_ERROR;
    }

    command = (RMTCommand *)dictGetVal(di);
    if(command == NULL){
        return RMT_ERROR;
    }

    if(command->flag & CMD_FLAG_NEED_CONFIRM){
        log_stdout("Do you really want to execute the \"%s\"?", command->name);
        char confirm_input[5] = {0};
        int confirm_retry = 0;

        while(strcmp("yes", confirm_input)){
            if(strcmp("no", confirm_input) == 0){
                goto done;
            }

            if(confirm_retry > 3){
                log_stdout("ERR: Your input is always error!");
                goto done;
            }
            
            memset(confirm_input, '\0', 5);
            
            log_stdout("please input \"yes\" or \"no\" :");
            scanf("%s", confirm_input);
            confirm_retry ++;
        }
    }

    args_num = (int)array_n(&ctx->args);
    if(args_num < command->min_arg_count || args_num > command->max_arg_count){
        if(command->max_arg_count == 0){
            log_stdout("ERR: command [%s] can not have argumemts", ctx->cmd);
        }else if(command->max_arg_count == command->min_arg_count){
            log_stdout("ERR: command [%s] must have %d argumemts.", 
                ctx->cmd, command->min_arg_count);
        }else{
            log_stdout("ERR: the argumemts number for command [%s] must between %d and %d.", 
                ctx->cmd, command->min_arg_count, command->max_arg_count);
        }
		
        return RMT_ERROR;
    }

    command->proc(ctx, command->type);

done:

#ifdef RMT_MEMORY_TEST
    mbuf_used_deinit();
    msg_used_deinit();
#endif
    
    return RMT_OK;
}

