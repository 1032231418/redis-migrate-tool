
#include <rmt_core.h>

#include <time.h>
#include <signal.h>

static int read_thread_data_init(read_thread_data *rdata)
{
	if(rdata == NULL)
	{
		return RMT_ERROR;
	}

	rdata->thread_id = 0;
    rdata->finish_read_nodes = 0;
    rdata->nodes_count = 0;
    rdata->loop = NULL;
    rdata->nodes_data = NULL;

    rdata->loop = aeCreateEventLoop(1000);
    if(rdata->loop == NULL)
    {
    	log_error("error: create event loop failed");
        return RMT_ERROR;
    }
    
	rdata->nodes_data = listCreate();
	if(rdata->nodes_data == NULL)
	{
		log_error("create node list failed: out of memory");
		return RMT_ENOMEM;
	}
	
	return RMT_OK;
}

static void read_thread_data_deinit(read_thread_data *rdata)
{
	if(rdata == NULL)
	{
		return;
	}

    if(rdata->loop != NULL)
	{
		aeDeleteEventLoop(rdata->loop);
		rdata->loop = NULL;
	}

	if(rdata->nodes_data != NULL)
	{
		listRelease(rdata->nodes_data);
		rdata->nodes_data = NULL;
	}
}

static void write_thread_data_deinit(write_thread_data *wdata);

static int write_thread_data_init(rmtContext *ctx, write_thread_data *wdata)
{
    int ret;
    
	if(wdata == NULL)
	{
		return RMT_ERROR;
	}

	wdata->thread_id = 0;
    wdata->finish_write_nodes = 0;
    wdata->nodes_count = 0;
	wdata->loop = NULL;
    wdata->trgroup = NULL;
    wdata->nodes = NULL;
    wdata->notice_pipe[0] = -1;
    wdata->notice_pipe[1] = -1;

	wdata->loop = aeCreateEventLoop(1000);
    if(wdata->loop == NULL)
    {
    	log_error("error: create event loop failed");
        goto error;
    }

    wdata->nodes = listCreate();
	if(wdata->nodes == NULL){
		log_error("Create node list failed: out of memory");
		goto error;
	}

    wdata->trgroup = target_group_create(ctx);
    if(wdata->trgroup == NULL){
        log_error("Target group create failed");
        goto error;
    }

    ret = pipe(wdata->notice_pipe);
    if(ret < 0)
    {
        log_error("Notice_pipe init failed: %s", strerror(errno));
        goto error;
    }

    ret = rmt_set_nonblocking(wdata->notice_pipe[0]);
    if(ret < 0)
    {
        log_error("Set notice_pipe[0] %d nonblock failed: %s", 
            wdata->notice_pipe[0], strerror(errno));
        goto error;
    }

    ret = rmt_set_nonblocking(wdata->notice_pipe[1]);
    if(ret < 0)
    {
        log_error("Set notice_pipe[1] %d nonblock failed: %s", 
            wdata->notice_pipe[1], strerror(errno));
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

    log_notice("Node count of source group : %d", node_count);
    log_notice("Total thread count : %d", thread_count);
    log_notice("Read thread count : %d", read_threads_count);
    log_notice("Write thread count : %d", write_threads_count);

    *read_threads = read_threads_count;
    *write_threads = write_threads_count;

    return RMT_OK;
}

static void read_threads_destroy(struct array *read_datas);

static struct array *read_threads_create(int read_threads_count, 
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
    int read_threads_hold_node_count = 0;
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

        read_data->nodes_count = step;
		for(k = begin; k < begin + step; k ++)
		{   
		    de = dictNext(di);
            if(de == NULL){
                log_error("Next node in the dict is NULL");
                goto error;
            }
            
            srnode = dictGetVal(de);
            if(srnode == NULL){
                log_error("Source redis node in the dict is NULL");
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

    //check the read threads
	for(i = 0; i < read_threads_count; i ++)
	{
		read_data = array_get(read_datas, (uint32_t)i);
        read_threads_hold_node_count += read_data->nodes_count;
	}

    if(read_threads_hold_node_count != node_count)
    {
        log_error("error: read threads hold node count is wrong");
        goto error;
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

static struct array *write_threads_create(rmtContext *ctx, int write_threads_count, 
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
    int write_threads_hold_node_count = 0;
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
        log_error("error: out of memory");
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

        write_data->nodes_count = step;
    	for(k = begin; k < begin + step; k ++)
    	{
    		de = dictNext(di);
            if(de == NULL){
                log_error("Next node in the dict is NULL");
                goto error;
            }
            
            srnode = dictGetVal(de);
            if(srnode == NULL){
                log_error("Source redis node in the dict is NULL");
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
                log_error("create readable notice event for node[%s] fd %d "
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

    //check the write threads
    for(i = 0; i < write_threads_count; i ++)
    {
    	write_data = array_get(write_datas, (uint32_t)i);
        write_threads_hold_node_count += write_data->nodes_count;
    }

    if(write_threads_hold_node_count != node_count)
    {
        log_error("error: write threads hold node count is wrong");
        goto error;
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
    write_thread_data *write_data = srnode->write_data;
    
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
            log_error("Create readable notice event for node[%s] fd %d "
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
        log_error("No redis nodes for this write thread %ld", 
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
                    log_error("Create sk_event for node[%s] failed: %s", 
                        srnode->addr, strerror(errno));
                    return;
                }
            }
            
            srnode->timestamp = rmt_msec_now();
            ret = aeCreateFileEvent(write_data->loop, srnode->sk_event, 
                AE_WRITABLE, redis_parse_rdb, srnode);
            if(ret == AE_ERR)
            {
                log_error("Create ae write event for node %s parse rdb file failed", 
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

static void send_data_to_target(aeEventLoop *el, int fd, void *privdata, int mask)
{
    redis_node *trnode = privdata;
    tcp_context *tc = trnode->tc;
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
            log_error("errors on connection with node[%s]", trnode->addr);

            //need reconnect to server
            aeStop(el);
        }

        if(n < (ssize_t)nsend){
            send_again = 0;
        }
    }else{
        n = 0;
    }

    nsent = n > 0 ? (size_t)n : 0;

    log_debug(LOG_DEBUG, "%u bytes has be sent", nsent);

    while((lnode_node = listFirst(&send_msgl)) != NULL){
        lnode_msg = listNodeValue(lnode_node);
        msg = listNodeValue(lnode_msg);
        listDelNode(&send_msgl, lnode_node);

        if (nsent == 0) {
            if (msg->mlen == 0) {
                //msg send done?
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
                msg_put(msg);
                msg_free(msg);
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
        log_error("out of memory");
        return;
    }

    mbuf = listLastValue(msg->data);
    if(mbuf == NULL || mbuf_full(mbuf)){
        mbuf = mbuf_get(mb);
        if(mbuf == NULL){
            log_error("mbuf_get NULL");
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
        log_error("response msg parsed error");
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
    write_thread_data *write_data = srnode->write_data;
    redis_group *trgroup = write_data->trgroup;
    tcp_context *tc;

    log_debug(LOG_DEBUG, "prepare_send_msg holds %u mbufs to node[%s]", 
        listLength(msg->data), trnode->addr);

    tc = trnode->tc;

    if(tc->sd < 0)
    {
        tc->flags &= ~RMT_BLOCK;    
        ret = rmt_tcp_context_connect_addr(tc, trnode->addr, 
            (int)rmt_strlen(trnode->addr), NULL, NULL);
        if(ret != RMT_OK)
        {
            log_error("connect to %s failed", trnode->addr);
            return RMT_ERROR;
        }

        log_debug(LOG_DEBUG, "ctx->noreply: %d", ctx->noreply);

        if(ctx->noreply == 0){
            ret = aeCreateFileEvent(write_data->loop, tc->sd, 
                AE_READABLE, recv_data_from_target, trnode);
            if(ret != AE_OK){
                log_error("send_data event create %ld failed: %s",
                    write_data->thread_id, strerror(errno));
                return RMT_ERROR;
            }

            log_debug(LOG_WARN, "node[%s] create read event for thread %ld", 
                trnode->addr, write_data->thread_id);
        }
    }

    listAddNodeTail(trnode->send_data, msg);
    ret = aeCreateFileEvent(write_data->loop, tc->sd, 
        AE_WRITABLE, send_data_to_target, trnode);
    if(ret != AE_OK)
    {
        log_error("send_data event create %ld failed: %s",
            write_data->thread_id, strerror(errno));
        return RMT_ERROR;
    }

    trgroup->msg_send_num ++;
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
    redis_group *trgroup = write_data->trgroup;
    list frag_msgl;
    uint32_t slots;
    redis_node *trnode;

    listInit(&frag_msgl);

    ASSERT(trgroup != NULL && trgroup->msg != NULL);
    
    msg = trgroup->msg;
    trgroup->msg = NULL;
    
    if(msg->noforward)
    {
        msg_put(msg);
        msg_free(msg);
        return RMT_OK;
    }

    slots = array_n(trgroup->route);
    ASSERT(slots > 0);

    ret = msg->fragment(trgroup, msg, slots, &frag_msgl);
    if (ret != RMT_OK) {
        log_error("msg fragment failed");
        goto error;
    }

    if (listLength(&frag_msgl) == 0) {

        kp = array_get(msg->keys, 0);
        trnode = trgroup->get_backend_node(trgroup, kp->start, (uint32_t)(kp->end-kp->start));
        if(prepare_send_msg(srnode, msg, trnode) != RMT_OK)
        {
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
        if(prepare_send_msg(srnode, sub_msg, trnode) != RMT_OK)
        {
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
    listNode *lnode;
    struct msg *req;
    
    if(trnode == NULL || resp == NULL){
        return RMT_ERROR;
    }

    msg_dump(resp, LOG_DEBUG);

    ASSERT(trnode->msg_rcv == resp);
    
    req = listPop(trnode->sent_data);
    ASSERT(req != NULL);
    ASSERT(req->sent == 1);
    ASSERT(req->peer == NULL);
    req->peer = resp;

    log_debug(LOG_DEBUG, "%d msgs wait for response from target group", listLength(trnode->sent_data));
    
    trnode->msg_rcv = NULL;

    ret = req->resp_check(req);
    if(ret != RMT_OK){
        log_error("Response check is error");
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

    if(rdb->type == REDIS_RDB_TYPE_FILE)
    {
        
        /*
        ret = redis_parse_rdb_file(srnode, -1);
        if(ret != RMT_OK)
        {
            log_error("redis node %s rdb file parse error", srnode->addr);
            aeDeleteFileEvent(write_data->loop, 
                srnode->notice_pipe[0], AE_READABLE);
            return;
        }
        */
        /*
            aeDeleteFileEvent(write_data->loop, srnode->notice_pipe[0], AE_READABLE);
        ret = aeCreateTimeEvent(write_data->loop, 0, redis_parse_rdb_time, srnode, NULL);
        if(ret == AE_ERR){
            log_error("Create ae time event for node %s redis_parse_rdb_time failed", 
                srnode->addr);
            return;
        }

        return;
        */


        aeDeleteFileEvent(write_data->loop, srnode->notice_pipe[0], AE_READABLE);
        
        if(srnode->sk_event < 0){
            srnode->sk_event = socket(AF_INET, SOCK_STREAM, 0);
            if(srnode->sk_event < 0){
                log_error("Create sk_event for node[%s] failed: %s", 
                    srnode->addr, strerror(errno));
                return;
            }
        }

        srnode->timestamp = rmt_msec_now();
        ret = aeCreateFileEvent(write_data->loop, srnode->sk_event, 
            AE_WRITABLE, redis_parse_rdb, srnode);
        if(ret == AE_ERR)
        {
            log_error("Create ae write event for node %s parse_request failed", 
                srnode->addr);
            return;
        }
        
        return;
        
    }

    aeDeleteFileEvent(write_data->loop, srnode->notice_pipe[0], AE_READABLE);
    
    ret = aeCreateFileEvent(write_data->loop, srnode->notice_pipe[0], 
        AE_READABLE, parse_request, srnode);
    if(ret != AE_OK)
    {
        log_error("Create ae read event for node %s parse_request failed", 
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
    size_t len;
    int data_type;

    RMT_NOTUSED(el);
    RMT_NOTUSED(fd);
    RMT_NOTUSED(privdata);
    RMT_NOTUSED(mask);

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
        log_error("recieve data node state is error: %d", rr->repl_state);
        return;
    }
    
    while(1){
        if(listLength(trgroup->piece_data) > 0){
            log_debug(LOG_VVERB,"trgroup->piece_data:");
            mbuf_list_dump(trgroup->piece_data, LOG_VVERB);
            mbuf_f = mbuf_list_pop(trgroup->piece_data);
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
    
        if(trgroup->msg == NULL){
            trgroup->msg = msg_get(srgroup->mb, 1, data_type);
            if(trgroup->msg != NULL){
                if(ctx->noreply){
                    trgroup->msg->noreply = 1;
                }else{
                    trgroup->msg->noreply = 0;
                }
            }
        }

        msg = trgroup->msg;
        if(msg == NULL){
            log_error("Out of memory");
            return;
        }

        if(msg->result == MSG_PARSE_REPAIR){
            mbuf_t = mbuf_f;

            if(listLength(trgroup->piece_data) > 0){
                mbuf_f = mbuf_list_pop(trgroup->piece_data);
            }else{
                if(rmt_read(fd,c,1) < 1){
                    //log_warn("read from notice sd failed");
                }
                
                mbuf_f = mttlist_pop(data);
            }

            if(mbuf_f == NULL){
                log_debug(LOG_DEBUG,"No more data to parse, wait for receive data.");
                mbuf_list_push_head(trgroup->piece_data, mbuf_t);
                break;
            }

            ASSERT(mbuf_t != NULL && 
                mbuf_storage_length(mbuf_t) > 0);

            len = MIN(mbuf_size(mbuf_t), mbuf_length(mbuf_f));

            ret = mbuf_move(mbuf_f, mbuf_t, (uint32_t)len);
            if(ret != RMT_OK){
                log_error("mbuf_f(%d) move data(%d) to mbuf_t(%d) failed",
                    mbuf_length(mbuf_f), len, mbuf_size(mbuf_t));
                return;
            }

            if(mbuf_empty(mbuf_f)){
                mbuf_put(mbuf_f);
            }else{
                mbuf_list_push(trgroup->piece_data, mbuf_f);
                
                log_hexdump(LOG_VVERB, mbuf_f->pos, mbuf_length(mbuf_f), 
                    "mbuf_f input mbuf_list");
            }

            listAddNodeTail(msg->data, mbuf_t);
            msg->pos = mbuf_t->pos;
        }else{
            listAddNodeTail(msg->data, mbuf_f);
            msg->pos = mbuf_f->pos;
        }

        msg_dump(msg, LOG_VVERB);
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

            nbuf = msg_split(msg, msg->pos);
            if (nbuf == NULL) {
                log_error("split msg failed: out of memory");
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
                log_error("split msg failed: out of memory");
                return;
            }

            msg->mlen -= mbuf_length(nbuf);
        }else if(msg->result == MSG_PARSE_AGAIN){
            log_debug(LOG_DEBUG, "msg %s parse again", 
                msg_type_string(msg->type));
            continue;
        }else{
            msg_put(trgroup->msg);
            msg_free(trgroup->msg);
            trgroup->msg = NULL;

            //need to handle the error
            
            continue;
        }

        ASSERT(listLength(trgroup->piece_data) <= 1);
        mbuf_list_push_head(trgroup->piece_data, nbuf);
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
        log_error("trnode->msg_rcv is null");
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
                log_error("response done error");
            }
            
            return RMT_OK;
        }

        nbuf = msg_split(msg, msg->pos);
        if (nbuf == NULL) {
            log_error("split msg failed: out of memory");
            return RMT_ERROR;
        }

        msg->mlen -= mbuf_length(nbuf);
        ASSERT(msg->mlen > 0);
        
        //check response
        ret = response_done(trnode, msg);
        if(ret != RMT_OK){
            log_error("response done error");
        }

        nmsg = msg_get(mb, 0, 0);
        if(nmsg == NULL){
            log_error("msg_get null");
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
            log_error("split msg failed: out of memory");
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
        log_error("Out of memory");
        goto error;
    }    

    if(type == GROUP_TYPE_RCLUSTER){
        ret = redis_cluster_init_from_addrs(rgroup, addrs);
        if(ret != RMT_OK){
            log_error("Init redis cluster from option failed");
            goto error;
        }

        rgroup->source = source;
        rgroup->kind = type;
        
        return rgroup;
    }

    if(type != GROUP_TYPE_SINGLE){
        log_error("Group type in the option must be single and redis cluster");
        goto error;
    }

    rgroup->kind = type;
    
    servers = sdssplitlen(addrs, (int)rmt_strlen(addrs),
        ADDRESS_SEPARATOR, rmt_strlen(ADDRESS_SEPARATOR), &servers_count);
    if(servers == NULL || servers_count <= 0)
    {
        log_error("address parsed error");
        goto error;
    }

    node_count = servers_count;

    ret = redis_group_init(ctx, rgroup, NULL, source);
    if(ret != RMT_OK){
        log_error("Init source group from option failed");
        goto error;
    }

    for(i = 0; i < servers_count; i ++){
        if(redis_group_add_node(rgroup, 
            servers[i], servers[i]) == NULL){
            log_error("Redis group add node[%s] failed",
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
            log_error("Out of memory");
            goto error;
        }

        ret = redis_group_init(ctx, srgroup, cp, 1);
        if(ret != RMT_OK){
            log_error("Source redis group init from conf file failed");
            goto error;
        }
    }

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
            log_error("Out of memory");
            goto error;
        }

        ret = redis_group_init(ctx, trgroup, cp, 0);
        if(ret != RMT_OK){
            log_error("Target redis group init from conf file failed");
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
    redis_group *srgroup = NULL;
    struct array *read_datas = NULL; //type : read_thread_data
    read_thread_data *read_data;
    struct array *write_datas = NULL; //type : write_thread_data
    write_thread_data *write_data;

    RMT_NOTUSED(type);

    if(ctx == NULL || ctx->source_addr == NULL){
        goto done;
    }

    signal(SIGPIPE, SIG_IGN);
        
    thread_count = ctx->thread_count;
    if(thread_count <= 0){
        log_error("error: thread count <= 0");
        return;
    }else if(thread_count == 1){
        thread_count ++;
	}

    srgroup = source_group_create(ctx);
    if(srgroup == NULL){
        log_error("Source redis group create failed");
        goto done;
    }

    node_count = (int)dictSize(srgroup->nodes);
    log_debug(LOG_DEBUG, "node count: %d", node_count);

    ret = assign_threads(node_count, thread_count, 
        &read_threads_count, &write_threads_count);
    if(ret != RMT_OK){
        log_error("Assign threads failed");
        goto done;
    }

    if(srgroup->kind == GROUP_TYPE_RDBFILE){
        read_threads_count = 0;
        write_threads_count = MIN(node_count,thread_count);
    }

    read_datas = read_threads_create(read_threads_count, 
        node_count, srgroup);
    if(read_datas == NULL && read_threads_count > 0){
        log_error("Read threads create failed");
        goto done;
    }

    write_datas = write_threads_create(ctx, write_threads_count, 
        node_count, srgroup);
    if(write_datas == NULL){
        log_error("Write threads create failed");
        goto done;
    }

    //run the read job
    for(i = 0; i < read_threads_count; i ++){
    	read_data = array_get(read_datas, (uint32_t)i);

        pthread_create(&read_data->thread_id, 
        	NULL, read_thread_run, read_data);
    }
    
    //run the write job
    for(i = 0; i < write_threads_count; i ++){
        write_data = array_get(write_datas, (uint32_t)i);

        pthread_create(&write_data->thread_id, 
            NULL, write_thread_run, write_data);
    }

    log_debug(LOG_NOTICE, "migrate job is running...");

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
        log_error("Command %s must have one argument" 
            " that is 'source' or 'target'.",
            ctx->cmd);
        return;
    }

    if(strcmp(array_get(&ctx->args, 0), "source") == 0){
        rgroup = source_group_create(ctx);
    }else if(strcmp(array_get(&ctx->args, 0), "target") == 0){
        rgroup = target_group_create(ctx);
    }else{
        log_error("Command %s must have one argument" 
            " that is 'source' or 'target'.",
            ctx->cmd);
        return;
    }

    if(rgroup == NULL){
        log_error("Group create failed");
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
    return memcmp(key1, key2, l1) == 0;
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

