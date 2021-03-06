/**
 * @file   msg.c
 * @author mathslinux <riegamaths@gmail.com>
 * @date   Thu Jun 14 14:42:17 2012
 * 
 * @brief  Message receive and send API
 * 
 * 
 */
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>

#include "type.h"
#include "smemory.h"
#include "json.h"
#include "http.h"
#include "url.h"
#include "logger.h"
#include "msg.h"
#include "queue.h"
#include "unicode.h"
#include "async.h"
#include "info.h"

static void *start_poll_msg(void *msg_list);
static void lwqq_recvmsg_poll_msg(struct LwqqRecvMsgList *list);
static json_t *get_result_json_object(json_t *json);
static int parse_recvmsg_from_json(LwqqRecvMsgList *list, const char *str);

static void lwqq_msg_message_free(void *opaque);
static void lwqq_msg_status_free(void *opaque);
static int msg_send_back(LwqqHttpRequest* req,void* data);
static int upload_cface_back(LwqqHttpRequest *req,void* data);
static int upload_offline_pic_back(LwqqHttpRequest* req,void* data);

/**
 * Create a new LwqqRecvMsgList object
 * 
 * @param client Lwqq Client reference
 * 
 * @return NULL on failure
 */
LwqqRecvMsgList *lwqq_recvmsg_new(void *client)
{
    LwqqRecvMsgList *list;

    list = s_malloc0(sizeof(*list));
    list->lc = client;
    pthread_mutex_init(&list->mutex, NULL);
    SIMPLEQ_INIT(&list->head);
    list->poll_msg = lwqq_recvmsg_poll_msg;
    
    return list;
}

/**
 * Free a LwqqRecvMsgList object
 * 
 * @param list 
 */
void lwqq_recvmsg_free(LwqqRecvMsgList *list)
{
    LwqqRecvMsg *recvmsg;
    
    if (!list)
        return ;

    pthread_mutex_lock(&list->mutex);
    while ((recvmsg = SIMPLEQ_FIRST(&list->head))) {
        SIMPLEQ_REMOVE_HEAD(&list->head, entries);
        lwqq_msg_free(recvmsg->msg);
        s_free(recvmsg);
    }
    pthread_mutex_unlock(&list->mutex);

    s_free(list);
    return ;
}

LwqqMsg *lwqq_msg_new(LwqqMsgType type)
{
    LwqqMsg *msg = NULL;
    LwqqMsgMessage* mmsg;

    msg = s_malloc0(sizeof(*msg));
    msg->type = type;

    switch (type) {
    case LWQQ_MT_BUDDY_MSG:
    case LWQQ_MT_GROUP_MSG:
        mmsg = s_malloc0(sizeof(LwqqMsgMessage));
        TAILQ_INIT(&mmsg->content);
        msg->opaque = mmsg;
        break;
    case LWQQ_MT_STATUS_CHANGE:
        msg->opaque = s_malloc0(sizeof(LwqqMsgStatusChange));
        break;
    case LWQQ_MT_KICK_MESSAGE:
        msg->opaque = s_malloc0(sizeof(LwqqMsgKickMessage));
        break;
    case LWQQ_MT_SYSTEM:
        msg->opaque = s_malloc0(sizeof(LwqqMsgSystem));
        break;
    case LWQQ_MT_BLIST_CHANGE:
        msg->opaque = s_malloc0(sizeof(LwqqMsgBlistChange));
        break;
    case LWQQ_MT_SYS_G_MSG:
        msg->opaque = s_malloc0(sizeof(LwqqMsgSysGMsg));
        break;
    default:
        lwqq_log(LOG_ERROR, "No such message type\n");
        goto failed;
        break;
    }

    return msg;
failed:
    lwqq_msg_free(msg);
    return NULL;
}

static void lwqq_msg_message_free(void *opaque)
{
    LwqqMsgMessage *msg = opaque;
    if (!msg) {
        return ;
    }

    s_free(msg->from);
    s_free(msg->to);
    s_free(msg->send);
    s_free(msg->f_name);
    s_free(msg->f_color);
    s_free(msg->group_code);
    s_free(msg->msg_id);

    LwqqMsgContent *c;
    TAILQ_FOREACH(c, &msg->content, entries) {
        switch(c->type){
            case LWQQ_CONTENT_STRING:
                s_free(c->data.str);
                break;
            case LWQQ_CONTENT_OFFPIC:
                s_free(c->data.img.file_path);
                s_free(c->data.img.name);
                s_free(c->data.img.data);
                break;
            case LWQQ_CONTENT_CFACE:
                s_free(c->data.cface.data);
                s_free(c->data.cface.name);
                s_free(c->data.cface.file_id);
                s_free(c->data.cface.key);
                break;
        }
        s_free(c);
    }
    
    s_free(msg);
}

static void lwqq_msg_status_free(void *opaque)
{
    LwqqMsgStatusChange *s = opaque;
    if (!s) {
        return ;
    }

    s_free(s->who);
    s_free(s);
}

/**
 * Free a LwqqMsg object
 * 
 * @param msg 
 */
void lwqq_msg_free(LwqqMsg *msg)
{
    if (!msg)
        return;

    printf ("type: %d\n", msg->type);
    if(msg->type==LWQQ_MT_GROUP_MSG||msg->type==LWQQ_MT_BUDDY_MSG)
        lwqq_msg_message_free(msg->opaque);
    else if(msg->type==LWQQ_MT_STATUS_CHANGE)
        lwqq_msg_status_free(msg->opaque);
    else if(msg->type==LWQQ_MT_KICK_MESSAGE){
        LwqqMsgKickMessage* kick;
        kick = msg->opaque;
        if(kick)
            s_free(kick->reason);
        s_free(kick);
    }
    else if(msg->type==LWQQ_MT_SYSTEM){
        LwqqMsgSystem* system;
        system = msg->opaque;
        if(system){
            s_free(system->seq);
            s_free(system->from_uin);
            s_free(system->account);
            s_free(system->msg);
            s_free(system->allow);
            s_free(system->stat);
            s_free(system->client_type);
        }
        s_free(system);
    }
    else if(msg->type==LWQQ_MT_BLIST_CHANGE){
        LwqqMsgBlistChange* blist;
        LwqqBuddy* buddy;
        LwqqBuddy* next;
        LwqqSimpleBuddy* simple;
        LwqqSimpleBuddy* simple_next;
        blist = msg->opaque;
        if(blist){
            simple = LIST_FIRST(&blist->added_friends);
            while(simple){
                simple_next = LIST_NEXT(simple,entries);
                lwqq_simple_buddy_free(simple);
                simple = simple_next;
            }
            buddy = LIST_FIRST(&blist->removed_friends);
            while(buddy){
                next = LIST_NEXT(buddy,entries);
                lwqq_buddy_free(buddy);
                buddy = next;
            }
        }
        s_free(blist);
    }
    else if(msg->type==LWQQ_MT_SYS_G_MSG){
        LwqqMsgSysGMsg* gmsg = msg->opaque;
        if(gmsg){
            s_free(gmsg->gcode);
        }
        s_free(gmsg);
    }else{
        lwqq_log(LOG_ERROR, "No such message type\n");
    }

    s_free(msg);
}

/**
 * Get the result object in a json object.
 * 
 * @param str
 * 
 * @return result object pointer on success, else NULL on failure.
 */
static json_t *get_result_json_object(json_t *json)
{
    json_t *json_tmp;
    char *value;

    /**
     * Frist, we parse retcode that indicate whether we get
     * correct response from server
     */
    value = json_parse_simple_value(json, "retcode");
    if (!value || strcmp(value, "0")) {
        goto failed ;
    }

    /**
     * Second, Check whether there is a "result" key in json object
     */
    json_tmp = json_find_first_label_all(json, "result");
    if (!json_tmp) {
        goto failed;
    }
    
    return json_tmp;

failed:
    return NULL;
}

static LwqqMsgType parse_recvmsg_type(json_t *json)
{
    LwqqMsgType type = LWQQ_MT_UNKNOWN;
    char *msg_type = json_parse_simple_value(json, "poll_type");
    if (!msg_type) {
        return type;
    }
    if (!strncmp(msg_type, "message", strlen("message"))) {
        type = LWQQ_MT_BUDDY_MSG;
    } else if (!strncmp(msg_type, "group_message", strlen("group_message"))) {
        type = LWQQ_MT_GROUP_MSG;
    } else if (!strncmp(msg_type, "buddies_status_change",
                        strlen("buddies_status_change"))) {
        type = LWQQ_MT_STATUS_CHANGE;
    } else if(!strncmp(msg_type,"kick_message",strlen("kick_message"))){
        type = LWQQ_MT_KICK_MESSAGE;
    } else if(!strncmp(msg_type,"system_message",strlen("system_message"))){
        type = LWQQ_MT_SYSTEM;
    }else if(!strncmp(msg_type,"buddylist_change",strlen("buddylist_change"))){
        type = LWQQ_MT_BLIST_CHANGE;
    }else if(!strncmp(msg_type,"sys_g_msg",strlen("sys_g_msg"))){
        type = LWQQ_MT_SYS_G_MSG;
    }else
        type = LWQQ_MT_UNKNOWN;
    return type;
}
static char* parse_escape(char* str)
{
    char* read = str;
    char* write = str;
    char* esc = NULL;
    while(read!='\0'){
        esc = strchr(read,'\\');
        if(esc==NULL){
            esc = strchr(read,'\0');
            esc++;
            memmove(write,read,esc-read);
            break;
        }
        memmove(write,read,esc-read);
        write += esc-read;
        switch(*(esc+1)){
            case 'n':
                *(write++) = '\n';
                break;
            case 'r':
                *(write++) = '\n';
                //processing \r\n.
                //let esc advance 2 then read ptr is set to currectly.
                if(*(esc+2)=='\\' &&*(esc+3)=='n')
                    esc=esc+2;
                break;
            case 't':
                *(write++) = '\t';
                break;
            case '\\':
                *(write++) = '\\';
        }
        read = esc+2;
    }
    return str;
}
static int parse_content(json_t *json, void *opaque)
{
    json_t *tmp, *ctent;
    LwqqMsgMessage *msg = opaque;

    tmp = json_find_first_label_all(json, "content");
    if (!tmp || !tmp->child || !tmp->child) {
        return -1;
    }
    tmp = tmp->child->child;
    for (ctent = tmp; ctent != NULL; ctent = ctent->next) {
        if (ctent->type == JSON_ARRAY) {
            /* ["font",{"size":10,"color":"000000","style":[0,0,0],"name":"\u5B8B\u4F53"}] */
            char *buf;
            /* FIXME: ensure NULL access */
            buf = ctent->child->text;
            if (!strcmp(buf, "font")) {
                const char *name, *color, *size;
                int sa, sb, sc;
                /* Font name */
                name = json_parse_simple_value(ctent, "name");
                name = name ?: "Arial";
                msg->f_name = ucs4toutf8(name);

                /* Font color */
                color = json_parse_simple_value(ctent, "color");
                color = color ?: "000000";
                msg->f_color = s_strdup(color);

                /* Font size */
                size = json_parse_simple_value(ctent, "size");
                size = size ?: "12";
                msg->f_size = atoi(size);

                /* Font style: style":[0,0,0] */
                tmp = json_find_first_label_all(ctent, "style");
                if (tmp) {
                    json_t *style = tmp->child->child;
                    const char *stylestr = style->text;
                    sa = (int)strtol(stylestr, NULL, 10);
                    style = style->next;
                    stylestr = style->text;
                    sb = (int)strtol(stylestr, NULL, 10);
                    style = style->next;
                    stylestr = style->text;
                    sc = (int)strtol(stylestr, NULL, 10);
                } else {
                    sa = 0;
                    sb = 0;
                    sc = 0;
                }
                msg->f_style.b = sa;
                msg->f_style.i = sb;
                msg->f_style.u = sc;
            } else if (!strcmp(buf, "face")) {
                /* ["face", 107] */
                /* FIXME: ensure NULL access */
                int facenum = (int)strtol(ctent->child->next->text, NULL, 10);
                LwqqMsgContent *c = s_malloc0(sizeof(*c));
                c->type = LWQQ_CONTENT_FACE;
                c->data.face = facenum; 
                TAILQ_INSERT_TAIL(&msg->content, c, entries);
            } else if(!strcmp(buf, "offpic")) {
                //["offpic",{"success":1,"file_path":"/d65c58ae-faa6-44f3-980e-272fb44a507f"}]
                LwqqMsgContent *c = s_malloc0(sizeof(*c));
                c->type = LWQQ_CONTENT_OFFPIC;
                c->data.img.success = atoi(json_parse_simple_value(ctent,"success"));
                c->data.img.file_path = s_strdup(json_parse_simple_value(ctent,"file_path"));
                TAILQ_INSERT_TAIL(&msg->content,c,entries);
            } else if(!strcmp(buf,"cface")){
                //["cface",{"name":"0C3AED06704CA9381EDCC20B7F552802.jPg","file_id":914490174,"key":"YkC3WaD3h5pPxYrY","server":"119.147.15.201:443"}]
                //["cface","0C3AED06704CA9381EDCC20B7F552802.jPg",""]
                LwqqMsgContent* c = s_malloc0(sizeof(*c));
                c->type = LWQQ_CONTENT_CFACE;
                c->data.cface.name = s_strdup(json_parse_simple_value(ctent,"name"));
                if(c->data.cface.name!=NULL){
                    c->data.cface.file_id = s_strdup(json_parse_simple_value(ctent,"file_id"));
                    c->data.cface.key = s_strdup(json_parse_simple_value(ctent,"key"));
                    char* server = s_strdup(json_parse_simple_value(ctent,"server"));
                    char* split = strchr(server,':');
                    strncpy(c->data.cface.serv_ip,server,split-server);
                    strncpy(c->data.cface.serv_port,split+1,strlen(split+1));
                }else{
                    c->data.cface.name = s_strdup(ctent->child->next->text);
                }
                TAILQ_INSERT_TAIL(&msg->content,c,entries);
            }
        } else if (ctent->type == JSON_STRING) {
            LwqqMsgContent *c = s_malloc0(sizeof(*c));
            c->type = LWQQ_CONTENT_STRING;
            c->data.str = json_unescape(ctent->text);
            //c->data.str = ucs4toutf8(ctent->text);
            //c->data.str = parse_escape(c->data.str);
            TAILQ_INSERT_TAIL(&msg->content, c, entries);
        }
    }

    /* Make msg valid */
    if (!msg->f_name || !msg->f_color || TAILQ_EMPTY(&msg->content)) {
        return -1;
    }
    if (msg->f_size < 10) {
        msg->f_size = 10;
    }

    return 0;
}

/**
 * {"poll_type":"message","value":{"msg_id":5244,"from_uin":570454553,
 * "to_uin":75396018,"msg_id2":395911,"msg_type":9,"reply_ip":176752041,
 * "time":1339663883,"content":[["font",{"size":10,"color":"000000",
 * "style":[0,0,0],"name":"\u5B8B\u4F53"}],"hello\n "]}}
 * 
 * @param json
 * @param opaque
 * 
 * @return
 */
static int parse_new_msg(json_t *json, void *opaque)
{
    LwqqMsgMessage *msg = opaque;
    char *time;
    
    msg->from = s_strdup(json_parse_simple_value(json, "from_uin"));
    if (!msg->from) {
        return -1;
    }

    time = json_parse_simple_value(json, "time");
    time = time ?: "0";
    msg->time = (time_t)strtoll(time, NULL, 10);
    msg->to = s_strdup(json_parse_simple_value(json, "to_uin"));
    msg->msg_id = s_strdup(json_parse_simple_value(json,"msg_id"));

    //if it failed means it is not group message.
    //so it equ NULL.
    msg->send = s_strdup(json_parse_simple_value(json, "send_uin"));
    msg->group_code = s_strdup(json_parse_simple_value(json,"group_code"));

    if (!msg->to) {
        return -1;
    }
    
    if (parse_content(json, opaque)) {
        return -1;
    }

    return 0;
}

/**
 * {"poll_type":"buddies_status_change",
 * "value":{"uin":570454553,"status":"offline","client_type":1}}
 * 
 * @param json
 * @param opaque
 * 
 * @return 
 */
static int parse_status_change(json_t *json, void *opaque)
{
    LwqqMsgStatusChange *msg = opaque;
    char *c_type;

    msg->who = s_strdup(json_parse_simple_value(json, "uin"));
    if (!msg->who) {
        return -1;
    }
    msg->status = s_strdup(json_parse_simple_value(json, "status"));
    if (!msg->status) {
        return -1;
    }
    c_type = json_parse_simple_value(json, "client_type");
    c_type = c_type ?: "1";
    msg->client_type = atoi(c_type);

    return 0;
}
static int parse_kick_message(json_t *json,void *opaque)
{
    LwqqMsgKickMessage *msg = opaque;
    char* show;
    show = json_parse_simple_value(json,"show_reason");
    if(show)msg->show_reason = atoi(show);
    msg->reason = ucs4toutf8(json_parse_simple_value(json,"reason"));
    if(!msg->reason){
        if(!show) msg->show_reason = 0;
        else return -1;
    }
    return 0;
}
static int parse_system_message(json_t *json,void* opaque)
{
    LwqqMsgSystem* system = opaque;
    system->seq = s_strdup(json_parse_simple_value(json,"seq"));
    const char* type = json_parse_simple_value(json,"type");
    if(strcmp(type,"verify_required")==0) system->type = VERIFY_REQUIRED;
    else system->type = SYSTEM_TYPE_UNKNOW;
    system->from_uin = s_strdup(json_parse_simple_value(json,"from_uin"));
    system->account = s_strdup(json_parse_simple_value(json,"account"));
    system->msg = s_strdup(json_parse_simple_value(json,"msg"));
    system->allow = s_strdup(json_parse_simple_value(json,"allow"));
    system->stat = s_strdup(json_parse_simple_value(json,"stat"));
    system->client_type = s_strdup(json_parse_simple_value(json,"client_type"));
    return 0;
}
static int parse_blist_change(json_t* json,void* opaque,void* _lc)
{
    LwqqClient* lc = _lc;
    LwqqMsgBlistChange* change = opaque;
    LwqqBuddy* buddy;
    LwqqSimpleBuddy* simple;
    json_t* ptr = json_find_first_label_all(json,"added_friends");
    ptr = ptr->child->child;
    while(ptr!=NULL){
        simple = lwqq_simple_buddy_new();
        simple->uin = s_strdup(json_parse_simple_value(ptr,"uin"));
        simple->cate_index = s_strdup(json_parse_simple_value(ptr,"groupid"));
        LIST_INSERT_HEAD(&change->added_friends,simple,entries);
        buddy = lwqq_buddy_new();
        buddy->uin = s_strdup(json_parse_simple_value(ptr,"uin"));
        buddy->cate_index = s_strdup(json_parse_simple_value(ptr,"groupid"));
        lwqq_info_get_friend_detail_info(lc,buddy,NULL);
        LIST_INSERT_HEAD(&lc->friends,buddy,entries);
        lwqq_info_get_friend_qqnumber(lc,buddy->uin);
        ptr = ptr->next;
    }
    ptr = json_find_first_label_all(json,"removed_friends");
    ptr = ptr->child->child;
    while(ptr!=NULL){
        const char* uin = json_parse_simple_value(ptr,"uin");
        ptr = ptr->next;

        buddy = lwqq_buddy_find_buddy_by_uin(lc,uin);
        if(buddy == NULL) continue;
        LIST_REMOVE(buddy,entries);
        LIST_INSERT_HEAD(&change->removed_friends,buddy,entries);
    }
    return 0;
}
static int parse_sys_g_msg(json_t *json,void* opaque)
{
    /*group create
      {"retcode":0,"result":[{"poll_type":"sys_g_msg","value":{"msg_id":39194,"from_uin":1528509098,"to_uin":350512021,"msg_id2":539171,"msg_type":38,"reply_ip":176752410,"type":"group_create","gcode":2676780935,"t_gcode":249818602,"owner_uin":350512021}}]}

      group join
      {"retcode":0,"result":[{"poll_type":"sys_g_msg","value":{"msg_id":5940,"from_uin":370154409,"to_uin":2501542492,"msg_id2":979390,"msg_type":33,"reply_ip":176498394,"type":"group_join","gcode":2570026216,"t_gcode":249818602,"op_type":3,"new_member":2501542492,"t_new_member":"","admin_uin":1448380605,"admin_nick":"\u521B\u5EFA\u8005"}}]}

      group leave
      {"retcode":0,"result":[{"poll_type":"sys_g_msg","value":{"msg_id":51488,"from_uin":1528509098,"to_uin":350512021,"msg_id2":180256,"msg_type":34,"reply_ip":176882139,"type":"group_leave","gcode":2676780935,"t_gcode":249818602,"op_type":2,"old_member":574849996,"t_old_member":""}}]}
      */
    LwqqMsgSysGMsg* msg = opaque;
    const char* type = json_parse_simple_value(json,"type");
    if(strcmp(type,"group_create")==0)msg->type = GROUP_CREATE;
    else if(strcmp(type,"group_join")==0)msg->type = GROUP_JOIN;
    else if(strcmp(type,"group_leave")==0)msg->type = GROUP_LEAVE;
    else msg->type = GROUP_UNKNOW;
    msg->gcode = s_strdup(json_parse_simple_value(json,"gcode"));
    return 0;

}
const char* get_host_of_url(const char* url,char* buffer)
{
    const char* ptr;
    const char* end;
    ptr = strstr(url,"://");
    if (ptr == NULL) return NULL;
    ptr+=3;
    end = strstr(ptr,"/");
    if(end == NULL)
        strcpy(buffer,ptr);
    else
        strncpy(buffer,ptr,end-ptr);
    return buffer;
}
static void request_content_offpic(LwqqClient* lc,const char* f_uin,LwqqMsgContent* c)
{
    LwqqHttpRequest* req;
    LwqqErrorCode error;
    LwqqErrorCode *err = &error;
    char* cookies;
    int ret;
    char url[512];
    char *file_path = url_encode(c->data.img.file_path);
    //there are face 1 to face 10 server to accelerate speed.
    snprintf(url, sizeof(url),
             "%s/get_offpic2?file_path=%s&f_uin=%s&clientid=%s&psessionid=%s",
             "http://d.web2.qq.com/channel",
             file_path,f_uin,lc->clientid,lc->psessionid);
    s_free(file_path);
    req = lwqq_http_create_default_request(url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://web2.qq.com/");
    req->set_header(req,"Host","d.web2.qq.com");

    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    ret = req->do_request(req, 0, NULL);

    if(ret||(req->http_code!=200)){
        if(err){
            *err = LWQQ_EC_HTTP_ERROR;
            goto done;
        }
    }

    c->data.img.data = req->response;
    c->data.img.size = req->resp_len;
    req->response = NULL;
    
done:
    lwqq_http_request_free(req);
    return;
}
static void request_content_cface(LwqqClient* lc,const char* group_code,const char* send_uin,LwqqMsgContent* c)
{
    LwqqHttpRequest* req;
    LwqqErrorCode error;
    LwqqErrorCode *err = &error;
    char* cookies;
    int ret;
    char url[512];
/*http://web2.qq.com/cgi-bin/get_group_pic?type=0&gid=3971957129&uin=4174682545&rip=120.196.211.216&rport=9072&fid=2857831080&pic=71A8E53B7F678D035656FECDA1BD7F31.jpg&vfwebqq=762a8682d17931d0cc647515e570435bd82e3a4e957bd052faa9615192eb7a3c4f1719006a7176c1&t=1343130567*/
    snprintf(url, sizeof(url),
             "%s/get_group_pic?type=0&gid=%s&uin=%s&rip=%s&rport=%s&fid=%s&pic=%s&vfwebqq=%s&t=%ld",
             "http://web2.qq.com/cgi-bin",
             group_code,send_uin,c->data.cface.serv_ip,c->data.cface.serv_port,
             c->data.cface.file_id,c->data.cface.name,lc->vfwebqq,time(NULL));
    req = lwqq_http_create_default_request(url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://web2.qq.com/");
    ///this is very important!!!!!!!!!
    req->set_header(req, "Host", "web2.qq.com");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }

    ret = req->do_request(req,0,NULL);

    if(ret||req->http_code!=200){
        if(err){
            *err = LWQQ_EC_HTTP_ERROR;
            goto done;
        }
    }

    c->data.cface.data = req->response;
    c->data.cface.size = req->resp_len;
    req->response = NULL;
done:
    lwqq_http_request_free(req);
}
static void request_content_cface2(LwqqClient* lc,const char* msg_id,const char* from_uin,LwqqMsgContent* c)
{
    LwqqHttpRequest* req;
    LwqqErrorCode error;
    LwqqErrorCode *err = &error;
    char* cookies;
    int ret;
    char url[512];
/*http://d.web2.qq.com/channel/get_cface2?lcid=3588&guid=85930B6CCE38BDAEF176FA83F0491569.jpg&to=2217604723&count=5&time=1&clientid=6325200&psessionid=8368046764001d636f6e6e7365727665725f77656271714031302e3133342e362e31333800001c9b000000d8026e04009563e4146d0000000a403946423664616232666d00000028ceb438eb76f1bc88360fc303e9148cc5dac8652a7a4bb702ee6dcf9bb10adf571a48b8a76b599e44*/
    snprintf(url, sizeof(url),
             "%s/get_cface2?lcid=%s&to=%s&guid=%s&count=5&time=1&clientid=%s&psessionid=%s",
             "http://d.web2.qq.com/channel",
             msg_id,from_uin,c->data.cface.name,lc->clientid,lc->psessionid);
    req = lwqq_http_create_default_request(url, err);
    if (!req) {
        goto done;
    }
    curl_easy_setopt(req->req,CURLOPT_VERBOSE,1);
    req->set_header(req, "Referer", "http://web2.qq.com/");
    ///this is very important!!!!!!!!!
    //req->set_header(req, "Host", "d.web2.qq.com");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }

    ret = req->do_request(req,0,NULL);

    if(ret||(req->http_code!=302&&req->http_code!=200)){
        goto done;
    }


    c->data.cface.data = req->response;
    c->data.cface.size = req->resp_len;
    req->response = NULL;

done:
    lwqq_http_request_free(req);
}
static void request_msg_offpic(LwqqClient* lc,int type,LwqqMsgMessage* msg)
{
    LwqqMsgContent* c;
    TAILQ_FOREACH(c,&msg->content,entries){
        if(c->type == LWQQ_CONTENT_OFFPIC){
            request_content_offpic(lc,msg->from,c);
        }else if(c->type == LWQQ_CONTENT_CFACE){
            if(type == LWQQ_MT_BUDDY_MSG)
                request_content_cface2(lc,msg->msg_id,msg->from,c);
            else
                request_content_cface(lc,msg->group_code,msg->send,c);
        }
    }
}
/**
 * Parse message received from server
 * Buddy message:
 * {"retcode":0,"result":[{"poll_type":"message","value":{"msg_id":5244,"from_uin":570454553,"to_uin":75396018,"msg_id2":395911,"msg_type":9,"reply_ip":176752041,"time":1339663883,"content":[["font",{"size":10,"color":"000000","style":[0,0,0],"name":"\u5B8B\u4F53"}],"hello\n "]}}]}
 * 
 * Message for Changing online status:
 * {"retcode":0,"result":[{"poll_type":"buddies_status_change","value":{"uin":570454553,"status":"offline","client_type":1}}]}
 * 
 * 
 * @param list 
 * @param response 
 */
static int parse_recvmsg_from_json(LwqqRecvMsgList *list, const char *str)
{
    int ret;
    int retcode = 0;
    json_t *json = NULL, *json_tmp, *cur;

    ret = json_parse_document(&json, (char *)str);
    puts(str);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of friends error: %s\n", str);
        goto done;
    }
    const char* retcode_str = json_parse_simple_value(json,"retcode");
    if(retcode_str)
        retcode = atoi(retcode_str);

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", str);
        goto done;
    }

    if (!json_tmp->child || !json_tmp->child->child) {
        goto done;
    }

    /* make json_tmp point to first child of "result" */
    json_tmp = json_tmp->child->child;
    for (cur = json_tmp; cur != NULL; cur = cur->next) {
        LwqqMsg *msg = NULL;
        LwqqMsgType msg_type;
        int ret;
        
        msg_type = parse_recvmsg_type(cur);
        msg = lwqq_msg_new(msg_type);
        if (!msg) {
            continue;
        }

        switch (msg_type) {
        case LWQQ_MT_BUDDY_MSG:
        case LWQQ_MT_GROUP_MSG:
            ret = parse_new_msg(cur, msg->opaque);
            request_msg_offpic(list->lc,msg->type,msg->opaque);
            break;
        case LWQQ_MT_STATUS_CHANGE:
            ret = parse_status_change(cur, msg->opaque);
            break;
        case LWQQ_MT_KICK_MESSAGE:
            ret = parse_kick_message(cur,msg->opaque);
            break;
        case LWQQ_MT_SYSTEM:
            ret = parse_system_message(cur,msg->opaque);
            break;
        case LWQQ_MT_BLIST_CHANGE:
            ret = parse_blist_change(cur,msg->opaque,list->lc);
            break;
        case LWQQ_MT_SYS_G_MSG:
            ret = parse_sys_g_msg(cur,msg->opaque);
            break;
        default:
            ret = -1;
            lwqq_log(LOG_ERROR, "No such message type\n");
            break;
        }

        if (ret == 0) {
            LwqqRecvMsg *rmsg = s_malloc0(sizeof(*rmsg));
            rmsg->msg = msg;
            /* Parse a new message successfully, link it to our list */
            pthread_mutex_lock(&list->mutex);
            SIMPLEQ_INSERT_TAIL(&list->head, rmsg, entries);
            pthread_mutex_unlock(&list->mutex);
        } else {
            lwqq_msg_free(msg);
        }
    }
    
done:
    if (json) {
        json_free_value(&json);
    }
    return retcode;
}

/**
 * Poll to receive message.
 * 
 * @param list
 */
static void *start_poll_msg(void *msg_list)
{
    LwqqClient *lc;
    LwqqHttpRequest *req = NULL;  
    int ret;
    char *cookies;
    char *s;
    int retcode;
    char msg[1024];
    LwqqRecvMsgList *list;

    list = (LwqqRecvMsgList *)msg_list;
    lc = (LwqqClient *)(list->lc);
    if (!lc) {
        goto failed;
    }
    snprintf(msg, sizeof(msg), "{\"clientid\":\"%s\",\"psessionid\":\"%s\"}",
             lc->clientid, lc->psessionid);
    s = url_encode(msg);
    snprintf(msg, sizeof(msg), "r=%s", s);
    s_free(s);

    /* Create a POST request */
    char url[512];
    snprintf(url, sizeof(url), "%s/channel/poll2", "http://d.web2.qq.com");
    req = lwqq_http_create_default_request(url, NULL);
    if (!req) {
        goto failed;
    }
    req->set_header(req, "Referer", "http://d.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "application/x-www-form-urlencoded");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    while(1) {
        ret = req->do_request(req, 1, msg);
        printf("%ld\n",req->http_code);
        if (ret || req->http_code != 200) {
            continue;
        }
        retcode = parse_recvmsg_from_json(list, req->response);
        if(retcode == 121){
            lwqq_async_dispatch(lc,POLL_LOST_CONNECTION,NULL);
            break;
        }
        //if(retcode==121)
        //    lwqq_relogin(lc);
    }
failed:
    pthread_exit(NULL);
}

static void lwqq_recvmsg_poll_msg(LwqqRecvMsgList *list)
{
    
    pthread_attr_init(&list->attr);
    pthread_attr_setdetachstate(&list->attr, PTHREAD_CREATE_DETACHED);

    pthread_create(&list->tid, &list->attr, start_poll_msg, list);
}

static void parse_unescape(char* source,char *buf,int buf_len)
{
    char* ptr = source;
    size_t idx;
    while(*ptr!='\0'){
        idx = strcspn(ptr,"\n\t\\");
        if(ptr[idx] == '\0'){
            strcpy(buf,ptr);
            buf+=idx;
            break;
        }
        strncpy(buf,ptr,idx);
        buf+=idx;
        switch(ptr[idx]){
            //note buf point the end position
            case '\n': strcpy(buf,"\\\\n");break;
            case '\t': strcpy(buf,"\\\\t");break;
            case '\\': strcpy(buf,"\\\\\\\\");break;
        }
        ptr+=idx+1;
        buf+=strlen(buf);
    }
    *buf = '\0';
}
#define LEFT "\\\""
#define RIGHT "\\\""
#define KEY(key) "\\\""key"\\\""

static char* content_parse_string(LwqqMsgMessage* msg,int msg_type,int *has_cface)
{
    //not thread safe. 
    //you need ensure only one thread send msg in one time.
    static char buf[2048];
    strcpy(buf,"\"[");
    LwqqMsgContent* c;

    TAILQ_FOREACH(c,&msg->content,entries){
        switch(c->type){
            case LWQQ_CONTENT_FACE:
                format_append(buf,"["KEY("face")",%d],",c->data.face);
                break;
            case LWQQ_CONTENT_OFFPIC:
                format_append(buf,"["KEY("offpic")","KEY("%s")","KEY("%s")",%ld],",
                        c->data.img.file_path,
                        c->data.img.name,
                        c->data.img.size);
                break;
            case LWQQ_CONTENT_CFACE:
                //[\"cface\",\"group\",\"0C3AED06704CA9381EDCC20B7F552802.jPg\"]
                if(msg_type == LWQQ_MT_GROUP_MSG)
                    format_append(buf,"["KEY("cface")","KEY("group")","KEY("%s")"],",
                            c->data.cface.name);
                else if(msg_type == LWQQ_MT_BUDDY_MSG)
                    format_append(buf,"["KEY("cface")","KEY("%s")"],",
                            c->data.cface.name);
                *has_cface = 1;
                break;
            case LWQQ_CONTENT_STRING:
                //format_append(buf,KEY("%s")",",c->data.str);
                strcat(buf,LEFT);
                parse_unescape(c->data.str,buf+strlen(buf),sizeof(buf)-strlen(buf));
                strcat(buf,RIGHT",");
                break;
        }
    }
    snprintf(buf+strlen(buf),sizeof(buf)-strlen(buf),
            "["KEY("font")",{"
            KEY("name")":"KEY("%s")","
            KEY("size")":"KEY("%d")","
            KEY("style")":[%d,%d,%d],"
            KEY("color")":"KEY("%s")
            "}]]\"",
            msg->f_name,msg->f_size,
            msg->f_style.b,msg->f_style.i,msg->f_style.u,
            msg->f_color);
    return buf;
}

LwqqAsyncEvent* lwqq_msg_upload_offline_pic(LwqqClient* lc,const char* to,LwqqMsgContent* c)
{
    if(c->type != LWQQ_CONTENT_OFFPIC) return NULL;
    if(!c->data.img.data || !c->data.img.name || !c->data.img.size) return NULL;

    LwqqHttpRequest *req;
    LwqqErrorCode err;
    const char* filename = c->data.img.name;
    const char* buffer = c->data.img.data;
    c->data.img.data = NULL;
    size_t size = c->data.img.size;
    char url[512];
    char *cookies;

    snprintf(url,sizeof(url),"http://weboffline.ftn.qq.com/ftn_access/upload_offline_pic?time=%ld",
            time(NULL));
    req = lwqq_http_create_default_request(url,&err);
    req->set_header(req,"Origin","http://web2.qq.com");
    req->set_header(req,"Referer","http://web2.qq.com/");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    req->add_form(req,LWQQ_FORM_CONTENT,"callback","parent.EQQ.Model.ChatMsg.callbackSendPic");
    req->add_form(req,LWQQ_FORM_CONTENT,"locallangid","2052");
    req->add_form(req,LWQQ_FORM_CONTENT,"clientversion","1409");
    req->add_form(req,LWQQ_FORM_CONTENT,"uin",lc->username);///<this may error
    req->add_form(req,LWQQ_FORM_CONTENT,"skey",lc->cookies->skey);
    req->add_form(req,LWQQ_FORM_CONTENT,"appid","1002101");
    req->add_form(req,LWQQ_FORM_CONTENT,"peeruin","593023668");///<what this means?
    req->add_file_content(req,"file",filename,buffer,size,NULL);
    req->add_form(req,LWQQ_FORM_CONTENT,"fileid","1");
    req->add_form(req,LWQQ_FORM_CONTENT,"vfwebqq",lc->vfwebqq);
    req->add_form(req,LWQQ_FORM_CONTENT,"senderviplevel","0");
    req->add_form(req,LWQQ_FORM_CONTENT,"reciverviplevel","0");

    return req->do_request_async(req,0,NULL,upload_offline_pic_back,c);
}
static int upload_offline_pic_back(LwqqHttpRequest* req,void* data)
{
    LwqqMsgContent* c = data;
    json_t* json = NULL;
    if(req->http_code!=200){
        goto done;
    }

    char *end = strchr(req->response,'}');
    *(end+1) = '\0';
    json_parse_document(&json,strchr(req->response,'{'));
    if(strcmp(json_parse_simple_value(json,"retcode"),"0")!=0){
        goto done;
    }
    c->type = LWQQ_CONTENT_OFFPIC;
    c->data.img.size = atol(json_parse_simple_value(json,"filesize"));
    c->data.img.file_path = s_strdup(json_parse_simple_value(json,"filepath"));
    s_free(c->data.img.name);
    c->data.img.name = s_strdup(json_parse_simple_value(json,"filename"));
    c->data.img.data = NULL;
done:
    if(json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;
}
static int query_gface_sig(LwqqClient* lc)
{
    LwqqHttpRequest *req;
    LwqqErrorCode err;
    char url[512];
    char *cookies;
    int ret;
    int succ = 0;
    if(lc->gface_key&&lc->gface_sig){
        return 1;
    }

    //https://d.web2.qq.com/channel/get_gface_sig2?clientid=30179476&psessionid=8368046764001e636f6e6e7365727665725f77656271714031302e3132382e36362e31313500006158000000c4036e04005c821a956d0000000a4065466637416b7142666d00000028fdd28eddedb8dd0cd414fdcb13af93532615ebe10b93f55182189da5c557360fee73da41ebf0c9fc&t=1343198241175
    snprintf(url,sizeof(url),"%s/get_gface_sig2?clientid=%s&psessionid=%s&t=%ld",
            "https://d.web2.qq.com/channel",lc->clientid,lc->psessionid,time(NULL));
    req = lwqq_http_create_default_request(url,&err);
    req->set_header(req,"Host","d.web2.qq.com");
    req->set_header(req,"Referer","https://d.web2.qq.com/cfproxy.html?v=20110331002&callback=1");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }

    ret = req->do_request(req,0,NULL);
    if(ret||req->http_code !=200){
        goto done;
    }
    json_t* json = NULL;
    json_parse_document(&json,req->response);
    lc->gface_key = s_strdup(json_parse_simple_value(json,"gface_key"));
    lc->gface_sig = s_strdup(json_parse_simple_value(json,"gface_sig"));
    succ = 1;
    
done:
    if(json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return succ;
}
LwqqAsyncEvent* lwqq_msg_upload_cface(LwqqClient* lc,LwqqMsgType type,LwqqMsgContent* c)
{
    if(c->type != LWQQ_CONTENT_CFACE) return NULL;
    if(!c->data.cface.name || !c->data.cface.data || !c->data.cface.size) return NULL;
    const char *filename = c->data.cface.name;
    const char *buffer = c->data.cface.data;
    c->data.cface.data = NULL;
    size_t size = c->data.cface.size;
    LwqqHttpRequest *req;
    LwqqErrorCode err;
    char url[512];
    char *cookies;
    static int fileid = 1;
    char fileid_str[20];

    snprintf(url,sizeof(url),"http://up.web2.qq.com/cgi-bin/cface_upload?time=%ld",
            time(NULL));
    req = lwqq_http_create_default_request(url,&err);
    curl_easy_setopt(req->req,CURLOPT_VERBOSE,1);
    req->set_header(req,"Origin","http://web2.qq.com");
    req->set_header(req,"Referer","http://web2.qq.com/");
    //req->set_header(req,"Host","up.web2.qq.com");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    req->add_form(req,LWQQ_FORM_CONTENT,"vfwebqq",lc->vfwebqq);
    //this is special for group msg.it can upload over 250K
    req->add_form(req,LWQQ_FORM_CONTENT,"from","control");
    if(type == LWQQ_MT_GROUP_MSG){
        req->add_form(req,LWQQ_FORM_CONTENT,"f","EQQ.Model.ChatMsg.callbackSendPicGroup");
    } else if(type == LWQQ_MT_BUDDY_MSG){
        req->add_form(req,LWQQ_FORM_CONTENT,"f","EQQ.Model.ChatMsg.callbackSendPic");
    }
    req->add_file_content(req,"custom_face",filename,buffer,size,NULL);
    snprintf(fileid_str,sizeof(fileid_str),"%d",fileid++);
    //cface 上传是会占用自定义表情的空间的.这里的fileid是几就是占用第几个格子.
    req->add_form(req,LWQQ_FORM_CONTENT,"fileid","1");

    void **data = s_malloc0(sizeof(void*)*2);
    data[0] = lc;
    data[1] = c;
    return req->do_request_async(req,0,NULL,upload_cface_back,data);
}
static int upload_cface_back(LwqqHttpRequest *req,void* data)
{
    void **d = data;
    LwqqClient* lc = d[0];
    LwqqMsgContent *c = d[1];
    s_free(data);
    int ret;
    int errno = 0;
    char msg[256];

    if(req->response)puts(req->response);
    if(req->http_code!=200){
        errno = 1;
        goto done;
    }
    /*char *ptr = strchr(req->response,'}');
    *(ptr+1) = '\0';
    while((ptr = strchr(req->response,'\''))){
        *ptr = '"';
    }*/
    //json_parse_document(&json,strchr(req->response,'{'));
    //ret = atoi(json_parse_simple_value(json,"ret"));
    char* ptr = strstr(req->response,"({");
    if(ptr==NULL){
        errno = 1;
        goto done;
    }
    sscanf(ptr,"({'ret':%d,'msg':'%[^\']'",&ret,msg);
    if(ret !=0 && ret !=4){
        errno = 1;
        goto done;
    }
    c->type = LWQQ_CONTENT_CFACE;
    char *file = msg;
    char *pos;
    //force to cut down the filename
    if((pos = strchr(file,' '))){
        *pos = '\0';
    }
    s_free(c->data.cface.name);
    c->data.cface.name = s_strdup(file);
    c->data.cface.data = NULL;

    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mutex);
    if(!lc->gface_sig)
        query_gface_sig(lc);
    pthread_mutex_unlock(&mutex);
done:
    lwqq_http_request_free(req);
    return errno;
}
/** 
 * 
 * 
 * @param lc 
 * @param sendmsg 
 * 
 * @return 1 means ok
 *         0 means failed or send failed
 */
LwqqAsyncEvent* lwqq_msg_send(LwqqClient *lc, LwqqMsg *msg)
{
    LwqqHttpRequest *req = NULL;  
    char *cookies;
    char *content = NULL;
    char data[1024] = {0};
    LwqqMsgMessage *mmsg;
    const char *tonam;
    const char *apistr;
    int has_cface = 0;

    if (!msg || (msg->type != LWQQ_MT_BUDDY_MSG &&
                 msg->type != LWQQ_MT_GROUP_MSG)) {
        goto failed;
    }
    if(msg->type == LWQQ_MT_BUDDY_MSG){
        tonam = "to";
        apistr = "send_buddy_msg2";
    }else if(msg->type == LWQQ_MT_GROUP_MSG){
        tonam = "group_uin";
        apistr = "send_qun_msg2";
    }
    mmsg = msg->opaque;
    content = content_parse_string(mmsg,msg->type,&has_cface);
    format_append(data,"r={"
            "\"%s\":%s,",
            tonam,mmsg->to);
    if(has_cface&&msg->type == LWQQ_MT_GROUP_MSG){
        format_append(data,
                "\"group_code\":%s,"
                "\"key\":\"%s\","
                "\"sig\":\"%s\",",
                mmsg->group_code,lc->gface_key,lc->gface_sig);
    }
    format_append(data,
            "\"face\":0,"
            "\"content\":%s,"
            "\"msg_id\":%ld,"
            "\"clientid\":\"%s\","
            "\"psessionid\":\"%s\"}",
            content,lc->msg_id,lc->clientid,lc->psessionid);
    format_append(data,"&clientid=%s&psessionid=%s",lc->clientid,lc->psessionid);
    puts(data);

    /* Create a POST request */
    char url[512];
    snprintf(url, sizeof(url), "%s/channel/%s", "http://d.web2.qq.com",apistr);
    req = lwqq_http_create_default_request(url, NULL);
    if (!req) {
        goto failed;
    }
    req->set_header(req, "Referer", "http://d.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "application/x-www-form-urlencoded");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    
    return req->do_request_async(req, 1, data,msg_send_back,lc);

failed:
    lwqq_http_request_free(req);
    return NULL;
}
static int msg_send_back(LwqqHttpRequest* req,void* data)
{
    json_t *root = NULL;
    int ret;
    int errno = 0;
    if (req->http_code != 200) {
        errno = 1;
        goto failed;
    }
    puts(req->response);

    //we check result if ok return 1,fail return 0;
    ret = json_parse_document(&root,req->response);
    if(ret != JSON_OK) goto failed;
    const char* retcode = json_parse_simple_value(root,"retcode");
    if(!retcode){
        errno = 1;
        goto failed;
    }
    errno = atoi(retcode);
failed:
    if(root)
        json_free_value(&root);
    lwqq_http_request_free(req);
    return errno;
}

int lwqq_msg_send_simple(LwqqClient* lc,int type,const char* to,const char* message)
{
    if(!lc||!to||!message)
        return 0;
    int ret = 0;
    LwqqMsg *msg = lwqq_msg_new(type);
    LwqqMsgMessage *mmsg = msg->opaque;
    mmsg->to = s_strdup(to);
    mmsg->f_name = "宋体";
    mmsg->f_size = 13;
    mmsg->f_style.b = 0,mmsg->f_style.i = 0,mmsg->f_style.u = 0;
    mmsg->f_color = "000000";
    LwqqMsgContent * c = s_malloc(sizeof(*c));
    c->type = LWQQ_CONTENT_STRING;
    c->data.str = s_strdup(message);
    TAILQ_INSERT_TAIL(&mmsg->content,c,entries);

    LWQQ_SYNC(lwqq_msg_send(lc,msg));

    mmsg->f_name = NULL;
    mmsg->f_color = NULL;

    lwqq_msg_free(msg);

    return ret;
}

