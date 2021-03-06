/**
 * @file   info.c
 * @author mathslinux <riegamaths@gmail.com>
 * @date   Sun May 27 19:48:58 2012
 *
 * @brief  Fetch QQ information. e.g. friends information, group information.
 *
 *
 */

#include <string.h>
#include <stdlib.h>
//to enable strptime
#define __USE_XOPEN
#include <time.h>
#include <utime.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "info.h"
#include "url.h"
#include "logger.h"
#include "http.h"
#include "smemory.h"
#include "json.h"
#include "async.h"

static json_t *get_result_json_object(json_t *json);
static void create_post_data(LwqqClient *lc, char *buf, int buflen);
static LwqqAsyncEvent* get_friend_qqnumber(LwqqClient *lc, const char *uin);
static int get_friend_qqnumber_back(LwqqHttpRequest* request,void* data);
static int get_avatar_back(LwqqHttpRequest* req,void* data);
static int get_friends_info_back(LwqqHttpRequest* req,void* data);
static int get_online_buddies_back(LwqqHttpRequest* req,void* data);
static int get_group_name_list_back(LwqqHttpRequest* req,void* data);
static int group_detail_back(LwqqHttpRequest* req,void* data);
static int change_buddy_markname_back(LwqqHttpRequest* req,void* data);


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

/**
 * Just a utility function
 *
 * @param lc
 * @param buf
 * @param buflen
 */
static void create_post_data(LwqqClient *lc, char *buf, int buflen)
{
    char *s;
    char m[256];
    snprintf(m, sizeof(m), "{\"h\":\"hello\",\"vfwebqq\":\"%s\"}",
             lc->vfwebqq);
    s = url_encode(m);
    snprintf(buf, buflen, "r=%s", s);
    s_free(s);
}

/**
 * Parse friend category information
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_categories_child(LwqqClient *lc, json_t *json)
{
    LwqqFriendCategory *cate;
    json_t *cur;
    char *index, *sort, *name;

    /* Make json point category reference */
    while (json) {
        if (json->text && !strcmp(json->text, "categories")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        index = json_parse_simple_value(cur, "index");
        sort = json_parse_simple_value(cur, "sort");
        name = json_parse_simple_value(cur, "name");
        cate = s_malloc0(sizeof(*cate));
        if (index) {
            cate->index = atoi(index);
        }
        if (sort) {
            cate->sort = atoi(sort);
        }
        if (name) {
            cate->name = s_strdup(name);
        }

        /* Add to categories list */
        LIST_INSERT_HEAD(&lc->categories, cate, entries);
    }

    /* add the default category */
    cate = s_malloc0(sizeof(*cate));
    cate->index = 0;
    cate->name = s_strdup("My Friends");
    LIST_INSERT_HEAD(&lc->categories, cate, entries);
}

/**
 * Parse info child
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_info_child(LwqqClient *lc, json_t *json)
{
    LwqqBuddy *buddy;
    json_t *cur;

    /* Make json point "info" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "info")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        buddy = lwqq_buddy_new();
        buddy->face = s_strdup(json_parse_simple_value(cur, "face"));
        buddy->flag = s_strdup(json_parse_simple_value(cur, "flag"));
        buddy->nick = s_strdup(json_parse_simple_value(cur, "nick"));
        buddy->uin = s_strdup(json_parse_simple_value(cur, "uin"));

        /* Add to buddies list */
        LIST_INSERT_HEAD(&lc->friends, buddy, entries);
    }
}

/**
 * Parse marknames child
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_marknames_child(LwqqClient *lc, json_t *json)
{
    LwqqBuddy *buddy;
    char *uin, *markname;
    json_t *cur;

    /* Make json point "info" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "marknames")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "uin");
        markname = json_parse_simple_value(cur, "markname");
        if (!uin || !markname)
            continue;

        buddy = lwqq_buddy_find_buddy_by_uin(lc, uin);
        if (!buddy)
            continue;

        /* Free old markname */
        if (buddy->markname)
            s_free(buddy->markname);
        buddy->markname = s_strdup(markname);
    }
}

/**
 * Parse friends child
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_friends_child(LwqqClient *lc, json_t *json)
{
    LwqqBuddy *buddy;
    char *uin, *cate_index;
    json_t *cur;

    /* Make json point "info" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "friends")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

//    {"flag":0,"uin":1907104721,"categories":0}
    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        LwqqFriendCategory *c_entry;
        uin = json_parse_simple_value(cur, "uin");
        cate_index = json_parse_simple_value(cur, "categories");
        if (!uin || !cate_index)
            continue;

        buddy = lwqq_buddy_find_buddy_by_uin(lc, uin);
        if (!buddy)
            continue;

        LIST_FOREACH(c_entry, &lc->categories, entries) {
            if (c_entry->index == atoi(cate_index)) {
                c_entry->count++;
            }
        }
        buddy->cate_index = s_strdup(cate_index);
    }
}

/**
 * Get QQ friends information. These information include basic friend
 * information, friends group information, and so on
 *
 * @param lc
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_friends_info(LwqqClient *lc, LwqqErrorCode *err)
{
    char msg[256] = {0};
    LwqqHttpRequest *req = NULL;
    char *cookies;

    /* Create post data: {"h":"hello","vfwebqq":"4354j53h45j34"} */
    create_post_data(lc, msg, sizeof(msg));

    /* Create a POST request */
    char url[512];
    snprintf(url, sizeof(url), "%s/api/get_user_friends2", "http://s.web2.qq.com");
    req = lwqq_http_create_default_request(url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "application/x-www-form-urlencoded");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    return req->do_request_async(req, 1, msg,get_friends_info_back,lc);

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":{"friends":[{"flag":0,"uin":1907104721,"categories":0},{"flag":0,"uin":1745701153,"categories":0},{"flag":0,"uin":445284794,"categories":0},{"flag":0,"uin":4188952283,"categories":0},{"flag":0,"uin":276408653,"categories":0},{"flag":0,"uin":1526867107,"categories":0}],"marknames":[{"uin":276408653,"markname":""}],"categories":[{"index":1,"sort":1,"name":""},{"index":2,"sort":2,"name":""},{"index":3,"sort":3,"name":""}],"vipinfo":[{"vip_level":1,"u":1907104721,"is_vip":1},{"vip_level":1,"u":1745701153,"is_vip":1},{"vip_level":1,"u":445284794,"is_vip":1},{"vip_level":6,"u":4188952283,"is_vip":1},{"vip_level":0,"u":276408653,"is_vip":0},{"vip_level":1,"u":1526867107,"is_vip":1}],"info":[{"face":294,"flag":8389126,"nick":"","uin":1907104721},{"face":0,"flag":518,"nick":"","uin":1745701153},{"face":0,"flag":526,"nick":"","uin":445284794},{"face":717,"flag":8388614,"nick":"QQ","uin":4188952283},{"face":81,"flag":8389186,"nick":"Kernel","uin":276408653},{"face":0,"flag":2147484166,"nick":"Q","uin":1526867107}]}}
     *
     */
done:
    lwqq_http_request_free(req);
    return NULL;
}
static int get_friends_info_back(LwqqHttpRequest* req,void* data)
{
    json_t *json = NULL, *json_tmp;
    int ret;
    LwqqErrorCode error;
    LwqqErrorCode* err = &error;
    LwqqClient* lc = data;

    if (req->http_code != 200) {
        if (err)
            *err = LWQQ_EC_HTTP_ERROR;
        goto done;
    }
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of friends error: %s\n", req->response);
        if (err)
            *err = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        goto json_error;
    }
    /** It seems everything is ok, we start parsing information
     * now
     */
    if (json_tmp->child && json_tmp->child->child ) {
        json_tmp = json_tmp->child->child;

        /* Parse friend category information */
        parse_categories_child(lc, json_tmp);

        /**
         * Parse friends information.
         * Firse, we parse object's "info" child
         * Then, parse object's "marknames" child
         * Last, parse object's "friends" child
         */
        parse_info_child(lc, json_tmp);
        parse_marknames_child(lc, json_tmp);
        parse_friends_child(lc, json_tmp);
    }

done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;

json_error:
    if (err)
        *err = LWQQ_EC_ERROR;
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;
}

void lwqq_info_get_avatar(LwqqClient* lc,int isgroup,void* grouporbuddy)
{
    static int serv_id = 0;
    if(!lc||!grouporbuddy) return ;
    //there have avatar already do not repeat work;
    LwqqBuddy* buddy = NULL;
    LwqqGroup* group = NULL;
    LwqqErrorCode error;
    if(isgroup) group = grouporbuddy;
    else buddy = grouporbuddy;
    const char* qqnumber = (isgroup)?group->account:buddy->qqnumber;
    const char* uin = (isgroup)?group->code:buddy->uin;

    //to avoid chinese character
    //setlocale(LC_TIME,"en_US.utf8");
    //first we try to read from disk
    char path[32];
    time_t modify=0;
    if(qqnumber) {
        snprintf(path,sizeof(path),LWQQ_CACHE_DIR"%s",qqnumber);
        struct stat st = {0};
        //we read it last modify date
        stat(path,&st);
        modify = st.st_mtime;
    }
    //we send request if possible with modify time
    //to reduce download rate
    LwqqHttpRequest* req;
    char* cookies;
    char url[512];
    char host[32];
    int type = (isgroup)?4:1;
    //there are face 1 to face 10 server to accelerate speed.
    snprintf(host,sizeof(host),"face%d.qun.qq.com",++serv_id);
    serv_id %= 10;
    snprintf(url, sizeof(url),
             "http://%s/cgi/svr/face/getface?cache=0&type=%d&fid=0&uin=%s&vfwebqq=%s",
             host,type,uin, lc->vfwebqq);
    req = lwqq_http_create_default_request(url, &error);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://web2.qq.com/");
    req->set_header(req,"Host",host);

    //we ask server if it indeed need to update
    if(modify) {
        struct tm modify_tm;
        char buf[32];
        strftime(buf,sizeof(buf),"%a, %d %b %Y %H:%M:%S GMT",localtime_r(&modify,&modify_tm) );
        req->set_header(req,"If-Modified-Since",buf);
    }
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    void** array = s_malloc0(sizeof(void*)*4);
    array[0] = lc;
    array[1] = buddy;
    array[2] = group;
    req->do_request_async(req, 0, NULL,get_avatar_back,array);
done:
    return;
}
static int get_avatar_back(LwqqHttpRequest* req,void* data)
{
    void** array = data;
    LwqqClient* lc = array[0];
    LwqqBuddy* buddy = array[1];
    LwqqGroup* group = array[2];
    s_free(data);
    int isgroup = (group !=NULL);
    const char* qqnumber = (isgroup)?group->account:buddy->qqnumber;
    char** avatar = (isgroup)?&group->avatar:&buddy->avatar;
    size_t* len = (isgroup)?&group->avatar_len:&buddy->avatar_len;
    char path[32];
    int hasfile=0;
    size_t filesize=0;
    FILE* f;

    if(qqnumber) {
        snprintf(path,sizeof(path),LWQQ_CACHE_DIR"%s",qqnumber);
        struct stat st = {0};
        //we read it last modify date
        hasfile = !stat(path,&st);
        filesize = st.st_size;
    }

    if((req->http_code!=200 && req->http_code!=304)){
        goto done;
    }

    //ok we need update our cache because 
    //our cache outdate
    if(req->http_code != 304) {
        //we 'move' it instead of copy it
        *avatar = req->response;
        *len = req->resp_len;
        req->response = NULL;
        req->resp_len = 0;

        //we cache it to file
        if(qqnumber) {
            f = fopen(path,"w");
            if(f==NULL) {
                mkdir(LWQQ_CACHE_DIR,0777);
                f = fopen(path,"w");
            }

            fwrite(*avatar,1,*len,f);
            fclose(f);
            //we read last modify time from response header
            struct tm wtm = {0};
            strptime(req->get_header(req,"Last-Modified"),
                    "%a, %d %b %Y %H:%M:%S GMT",&wtm);
            //and write it to file
            struct utimbuf wutime;
            wutime.modtime = mktime(&wtm);
            wutime.actime = wutime.modtime;//it is not important
            utime(path,&wutime);
        }
        lwqq_http_request_free(req);
        if(isgroup)lwqq_async_dispatch(lc,GROUP_AVATAR,group);
        else lwqq_async_dispatch(lc,FRIEND_AVATAR,buddy);
        return 0;
    }

done:
    //failed or we do not need update
    //we read from file
    if(hasfile){
        f = fopen(path,"r");
        *avatar = s_malloc(filesize);
        fread(*avatar,1,filesize,f);
        *len = filesize;
    }
    lwqq_http_request_free(req);
    if(isgroup)lwqq_async_dispatch(lc,GROUP_AVATAR,group);
    else lwqq_async_dispatch(lc,FRIEND_AVATAR,buddy);
    return 0;
}

/**
 * Parsing group info like this.
 *
 * "gnamelist":[
 *  {"flag":17825793,"name":"EGE...C/C++............","gid":3772225519,"code":1713443374},
 *  {"flag":1,"name":"............","gid":2698833507,"code":3968641865}
 * ]
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_gnamelist_child(LwqqClient *lc, json_t *json)
{
    LwqqGroup *group;
    json_t *cur;

    /* Make json point "gnamelist" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "gnamelist")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        group = lwqq_group_new();
        group->flag = s_strdup(json_parse_simple_value(cur, "flag"));
        group->name = s_strdup(json_parse_simple_value(cur, "name"));
        group->gid = s_strdup(json_parse_simple_value(cur, "gid"));
        group->code = s_strdup(json_parse_simple_value(cur, "code"));

        /* we got the 'code', so we can get the qq group number now */
        //group->account = get_group_qqnumber(lc, group->code);

        /* Add to groups list */
        LIST_INSERT_HEAD(&lc->groups, group, entries);
    }
}

/**
 * Parse group info like this
 *
 * "gmarklist":[{"uin":2698833507,"markname":".................."}]
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_gmarklist_child(LwqqClient *lc, json_t *json)
{
    LwqqGroup *group;
    json_t *cur;
    char *uin;
    char *markname;

    /* Make json point "gmarklist" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "gmarklist")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "uin");
        markname = json_parse_simple_value(cur, "markname");

        if (!uin || !markname)
            continue;
        group = lwqq_group_find_group_by_gid(lc, uin);
        if (!group)
            continue;
        group->markname = s_strdup(markname);
    }
}

/**
 * Get QQ groups' name information. Get only 'name', 'gid' , 'code' .
 *
 * @param lc
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_group_name_list(LwqqClient *lc, LwqqErrorCode *err)
{

    lwqq_log(LOG_DEBUG, "in function.");

    char msg[256];
    char url[512];
    LwqqHttpRequest *req = NULL;
    char *cookies;

    /* Create post data: {"h":"hello","vfwebqq":"4354j53h45j34"} */
    create_post_data(lc, msg, sizeof(msg));

    /* Create a POST request */
    snprintf(url, sizeof(url), "%s/api/get_group_name_list_mask2", "http://s.web2.qq.com");
    req = lwqq_http_create_default_request(url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "application/x-www-form-urlencoded");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    return req->do_request_async(req, 1, msg,get_group_name_list_back,lc);
done:
    lwqq_http_request_free(req);
    return NULL;
}
static int get_group_name_list_back(LwqqHttpRequest* req,void* data)
{
    json_t *json = NULL, *json_tmp;
    int ret;
    LwqqClient* lc = data;
    LwqqErrorCode error;
    LwqqErrorCode* err = &error;

    if (req->http_code != 200) {
        if (err)
            *err = LWQQ_EC_HTTP_ERROR;
        goto done;
    }

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":{
     * "gmasklist":[],
     * "gnamelist":[
     *  {"flag":17825793,"name":"EGE...C/C++............","gid":3772225519,"code":1713443374},
     *  {"flag":1,"name":"............","gid":2698833507,"code":3968641865}
     * ],
     * "gmarklist":[{"uin":2698833507,"markname":".................."}]}}
     *
     */
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        if (err)
            *err = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        goto json_error;
    }

    /** It seems everything is ok, we start parsing information
     * now
     */
    if (json_tmp->child && json_tmp->child->child ) {
        json_tmp = json_tmp->child->child;

        /* Parse friend category information */
        parse_groups_gnamelist_child(lc, json_tmp);
        parse_groups_gmarklist_child(lc, json_tmp);

    }

done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;

json_error:
    if (err)
        *err = LWQQ_EC_ERROR;
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;
}


/**
 * Get all friends qqnumbers
 *
 * @param lc
 * @param err
 */
void lwqq_info_get_all_friend_qqnumbers(LwqqClient *lc, LwqqErrorCode *err)
{
    LwqqBuddy *buddy;

    if (!lc)
        return ;

    LIST_FOREACH(buddy, &lc->friends, entries) {
        if (!buddy->qqnumber) {
            /** If qqnumber hasnt been fetched(NB: lc->myself has qqnumber),
             * fetch it
             */
            get_friend_qqnumber(lc, buddy->uin);
            //lwqq_log(LOG_DEBUG, "Get buddy qqnumber: %s\n", buddy->qqnumber);
        }
    }

    if (err) {
        *err = LWQQ_EC_OK;
    }
}

/**
 * Get friend qqnumber
 *
 * @param lc
 * @param uin
 *
 * @return qqnumber on sucessful, NB: caller is responsible for freeing
 * the memory returned by this function
 */
LwqqAsyncEvent* lwqq_info_get_friend_qqnumber(LwqqClient *lc, const char *uin)
{
    return get_friend_qqnumber(lc, uin);
}

/**
 * Parse group info like this
 * Is the "members" in "ginfo" useful ? Here not parsing it...
 *
 * "result":{
 * "ginfo":
 *   {"face":0,"memo":"","class":10026,"fingermemo":"","code":3968641865,"createtime":1339647698,"flag":1,
 *    "level":0,"name":"............","gid":2698833507,"owner":909998471,
 *    "members":[{"muin":56360327,"mflag":0},{"muin":909998471,"mflag":0}],
 *    "option":2},
 *
 * @param lc
 * @param group
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_ginfo_child(LwqqClient *lc, LwqqGroup *group,  json_t *json)
{
    char *gid;

    /* Make json point "ginfo" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "ginfo")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    //json = json->child;    // there's no array here , comment it.
    gid = json_parse_simple_value(json,"gid");
    if (strcmp(group->gid, gid) != 0) {
        lwqq_log(LOG_ERROR, "Parse json object error.");
        return;
    }

#define  SET_GINFO(key, name) {                                    \
        if (group->key) {                                               \
            s_free(group->key);                                         \
        }                                                               \
        group->key = s_strdup(json_parse_simple_value(json, name));     \
    }

    /* we have got the 'code','name' and 'gid', so we comment it here. */
    SET_GINFO(face, "face");
    SET_GINFO(memo, "memo");
    SET_GINFO(class, "class");
    SET_GINFO(fingermemo,"fingermemo");
    //SET_GINFO(code, "code");
    SET_GINFO(createtime, "createtime");
    SET_GINFO(flag, "flag");
    SET_GINFO(level, "level");
    //SET_GINFO(name, "name");
    //SET_GINFO(gid, "gid");
    SET_GINFO(owner, "owner");
    SET_GINFO(option, "option");

#undef SET_GINFO

}

/**
 * Parse group members info
 * we only get the "nick" and the "uin", and get the members' qq number.
 *
 * "minfo":[
 *   {"nick":"evildoer","province":"......","gender":"male","uin":56360327,"country":"......","city":"......"},
 *   {"nick":"evil...doer","province":"......","gender":"male","uin":909998471,"country":"......","city":"......"}],
 *
 * @param lc
 * @param group
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_minfo_child(LwqqClient *lc, LwqqGroup *group,  json_t *json)
{
    LwqqBuddy *member;
    json_t *cur;
    char *uin;
    char *nick;

    /* Make json point "minfo" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "minfo")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "uin");
        nick = json_parse_simple_value(cur, "nick");

        if (!uin || !nick)
            continue;

        member = lwqq_buddy_new();

        member->uin = s_strdup(uin);
        member->nick = s_strdup(nick);

        // FIX ME: should we get group members qqnumber here ? 
        // we can get the members' qq number by uin 
        //member->qqnumber = get_friend_qqnumber(lc, member->uin);

        /* Add to members list */
        LIST_INSERT_HEAD(&group->members, member, entries);
    }
}

/**
 * mark qq group's online members
 *
 * "stats":[{"client_type":1,"uin":56360327,"stat":10},{"client_type":41,"uin":909998471,"stat":10}],
 *
 * @param lc
 * @param group
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_stats_child(LwqqClient *lc, LwqqGroup *group,  json_t *json)
{
    LwqqBuddy *member;
    json_t *cur;
    char *uin;

    /* Make json point "stats" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "stats")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "uin");

        if (!uin)
            continue;
        member = lwqq_group_find_group_member_by_uin(group, uin);
        if (!member)
            continue;
        member->client_type = s_strdup(json_parse_simple_value(cur, "client_type"));
        member->stat = s_strdup(json_parse_simple_value(cur, "stat"));

    }
}

/**
 * Get QQ groups detail information.
 *
 * @param lc
 * @param group
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_group_detail_info(LwqqClient *lc, LwqqGroup *group,
                                     LwqqErrorCode *err)
{
    lwqq_log(LOG_DEBUG, "in function.");

    char url[512];
    LwqqHttpRequest *req = NULL;
    int ret;
    char *cookies;

    if (!lc || ! group) {
        return NULL;
    }

    /* Make sure we know code. */
    if (!group->code) {
        if (err)
            *err = LWQQ_EC_NULL_POINTER;
        return NULL;
    }

    /* Create a GET request */
    snprintf(url, sizeof(url),
             "%s/api/get_group_info_ext2?gcode=%s&vfwebqq=%s",
             "http://s.web2.qq.com", group->code, lc->vfwebqq);
    req = lwqq_http_create_default_request(url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "utf-8");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    void **data = s_malloc0(sizeof(void*)*2);
    data[0] = lc;
    data[1] = group;
    return req->do_request_async(req, 0, NULL,group_detail_back,data);
done:
    lwqq_http_request_free(req);
    return NULL;
}

static int group_detail_back(LwqqHttpRequest* req,void* data)
{
    void **d = data;
    LwqqClient* lc = d[0];
    LwqqGroup* group = d[1];
    s_free(data);
    int ret;
    int errno = 0;
    json_t *json = NULL, *json_tmp;
    if (req->http_code != 200) {
        errno = LWQQ_EC_HTTP_ERROR;
        goto done;
    }

    /**
     * Here, we got a json object like this:
     *
     * {"retcode":0,
     * "result":{
     * "stats":[{"client_type":1,"uin":56360327,"stat":10},{"client_type":41,"uin":909998471,"stat":10}],
     * "minfo":[
     *   {"nick":"evildoer","province":"......","gender":"male","uin":56360327,"country":"......","city":"......"},
     *   {"nick":"evil...doer","province":"......","gender":"male","uin":909998471,"country":"......","city":"......"}
     *   {"nick":"glrapl","province":"","gender":"female","uin":1259717843,"country":"","city":""}],
     * "ginfo":
     *   {"face":0,"memo":"","class":10026,"fingermemo":"","code":3968641865,"createtime":1339647698,"flag":1,
     *    "level":0,"name":"............","gid":2698833507,"owner":909998471,
     *    "members":[{"muin":56360327,"mflag":0},{"muin":909998471,"mflag":0}],
     *    "option":2},
     * "cards":[{"muin":3777107595,"card":""},{"muin":3437728033,"card":":FooTearth"}],
     * "vipinfo":[{"vip_level":0,"u":56360327,"is_vip":0},{"vip_level":0,"u":909998471,"is_vip":0}]}}
     *
     */
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        errno = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        goto json_error;
    }

    /** It seems everything is ok, we start parsing information
     * now
     */
    if (json_tmp->child && json_tmp->child->child ) {
        json_tmp = json_tmp->child->child;

        /* first , get group information */
        parse_groups_ginfo_child(lc, group, json_tmp);
        /* second , get group members */
        parse_groups_minfo_child(lc, group, json_tmp);
        /* third , mark group's online members */
        parse_groups_stats_child(lc, group, json_tmp);

    }

done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return errno;

json_error:
    errno = LWQQ_EC_ERROR;
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return errno;
}

/**
 * Get QQ friend number
 *
 * @param lc
 * @param uin Friend's uin
 *
 * @return
 */
static LwqqAsyncEvent* get_friend_qqnumber(LwqqClient *lc, const char *uin)
{
    if (!lc || !uin) {
        return NULL;
    }

    char url[512];
    LwqqHttpRequest *req = NULL;
    json_t *json = NULL;
    //char *qqnumber = NULL;
    char *cookies;

    if (!lc || ! uin) {
        return NULL;
    }

    /* Create a GET request */
    /*     g_sprintf(params, GETQQNUM"?tuin=%s&verifysession=&type=1&code="
           "&vfwebqq=%s&t=%ld", uin */
    snprintf(url, sizeof(url),
             "%s/api/get_friend_uin2?tuin=%s&verifysession=&type=1&code=&vfwebqq=%s",
             "http://s.web2.qq.com", uin, lc->vfwebqq);
    req = lwqq_http_create_default_request(url, NULL);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "utf-8");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    return req->do_request_async(req, 0, NULL,get_friend_qqnumber_back,lc);
done:
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return NULL;
}
static int get_friend_qqnumber_back(LwqqHttpRequest* req,void* data)
{
    LwqqClient* lc = data;
    char* uin;
    char* account;
    int ret;
    json_t* json=NULL;
    int succ = 0;
    if (req->http_code != 200) {
        lwqq_log(LOG_ERROR, "qqnumber response error: %s\n", req->response);
        goto done;
    }

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":{"uiuin":"","account":615050000,"uin":954663841}}
     *
     */
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        goto done;
    }
    uin = json_parse_simple_value(json,"uin");
    account = json_parse_simple_value(json,"account");

    LwqqBuddy* buddy = lwqq_buddy_find_buddy_by_uin(lc,uin);
    if(buddy){
        buddy->qqnumber = s_strdup(account);
        succ = 1;
        lwqq_async_dispatch(lc,FRIEND_COME,buddy);
        goto done;
    }
    LwqqGroup* group = NULL;
    LwqqGroup* gp;
    LIST_FOREACH(gp,&lc->groups,entries){
        if(strcmp(gp->code,uin)==0)
            group = gp;
    }
    if(group){
        succ = 1;
        group->account = s_strdup(account);
        lwqq_async_dispatch(lc,GROUP_COME,group);
        goto done;
    }
done:
    if(succ==0){
        lwqq_log(LOG_ERROR,"fetch qqnumber error",req->response);
    }

    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;
}


/**
 * Get detail information of QQ friend(NB: include myself)
 * QQ server need us to pass param like:
 * tuin=244569070&verifysession=&code=&vfwebqq=e64da25c140c66
 *
 * @param lc
 * @param buddy
 * @param err
 */
void lwqq_info_get_friend_detail_info(LwqqClient *lc, LwqqBuddy *buddy,
                                      LwqqErrorCode *err)
{
    lwqq_log(LOG_DEBUG, "in function.");

    char url[512];
    LwqqHttpRequest *req = NULL;
    int ret;
    json_t *json = NULL, *json_tmp;
    char *cookies;

    if (!lc || ! buddy) {
        return ;
    }

    /* Make sure we know uin. */
    if (!buddy->uin) {
        if (err)
            *err = LWQQ_EC_NULL_POINTER;
        return ;
    }

    /* Create a GET request */
    snprintf(url, sizeof(url),
             "%s/api/get_friend_info2?tuin=%s&verifysession=&code=&vfwebqq=%s",
             "http://s.web2.qq.com", buddy->uin, lc->vfwebqq);
    req = lwqq_http_create_default_request(url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "utf-8");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    ret = req->do_request(req, 0, NULL);
    if (ret || req->http_code != 200) {
        if (err)
            *err = LWQQ_EC_HTTP_ERROR;
        goto done;
    }

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":{"face":519,"birthday":
     * {"month":9,"year":1988,"day":26},"occupation":"学生",
     * "phone":"82888909","allow":1,"college":"西北工业大学","reg_time":0,
     * "uin":1421032531,"constel":8,"blood":2,
     * "homepage":"http://www.ifeng.com","stat":10,"vip_info":0,
     * "country":"中国","city":"西安","personal":"给力啊~~","nick":"阿凡达",
     * "shengxiao":5,"email":"avata@126.com","client_type":41,
     * "province":"陕西","gender":"male","mobile":"139********"}}
     *
     */
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        if (err)
            *err = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        goto json_error;
    }

    /** It seems everything is ok, we start parsing information
     * now
     */
    if (json_tmp->child) {
        json_tmp = json_tmp->child;
#define  SET_BUDDY_INFO(key, name) {                                    \
            if (buddy->key) {                                           \
                s_free(buddy->key);                                     \
            }                                                           \
            buddy->key = s_strdup(json_parse_simple_value(json, name)); \
        }
        SET_BUDDY_INFO(uin, "uin");
        SET_BUDDY_INFO(face, "face");
        /* SET_BUDDY_INFO(birthday, "birthday"); */
        SET_BUDDY_INFO(occupation, "occupation");
        SET_BUDDY_INFO(phone, "phone");
        SET_BUDDY_INFO(allow, "allow");
        SET_BUDDY_INFO(college, "college");
        SET_BUDDY_INFO(reg_time, "reg_time");
        SET_BUDDY_INFO(constel, "constel");
        SET_BUDDY_INFO(blood, "blood");
        SET_BUDDY_INFO(homepage, "homepage");
        SET_BUDDY_INFO(stat, "stat");
        if(buddy->status) s_free(buddy->status);
        buddy->status = NULL;
        if(strcmp(buddy->stat,"10")==0)buddy->status = s_strdup("online");
        else if(strcmp(buddy->stat,"20")==0) buddy->status = s_strdup("offline");
        else if(strcmp(buddy->stat,"30")==0) buddy->status = s_strdup("busy");
        else if(strcmp(buddy->stat,"50")==0) buddy->status = s_strdup("away");
        SET_BUDDY_INFO(vip_info, "vip_info");
        SET_BUDDY_INFO(country, "country");
        SET_BUDDY_INFO(city, "city");
        SET_BUDDY_INFO(personal, "personal");
        SET_BUDDY_INFO(nick, "nick");
        SET_BUDDY_INFO(shengxiao, "shengxiao");
        SET_BUDDY_INFO(email, "email");
        SET_BUDDY_INFO(client_type, "client_type");
        SET_BUDDY_INFO(province, "province");
        SET_BUDDY_INFO(gender, "gender");
        SET_BUDDY_INFO(mobile, "mobile");
#undef SET_BUDDY_INFO
    }


done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return ;

json_error:
    if (err)
        *err = LWQQ_EC_ERROR;
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
}

static void update_online_buddies(LwqqClient *lc, json_t *json)
{
    /**
     * The json object is:
     * [{"uin":1100872453,"status":"online","client_type":21},"
     * "{"uin":2726159277,"status":"busy","client_type":1}]
     */
    json_t *cur;
    for (cur = json->child; cur != NULL; cur = cur->next) {
        char *uin, *status, *client_type;
        LwqqBuddy *b;
        uin = json_parse_simple_value(cur, "uin");
        status = json_parse_simple_value(cur, "status");
        if (!uin || !status) {
            continue;
        }
        client_type = json_parse_simple_value(cur, "client_type");
        b = lwqq_buddy_find_buddy_by_uin(lc, uin);
        if (b) {
            s_free(b->status);
            b->status = s_strdup(status);
            if (client_type) {
                s_free(b->client_type);
                b->client_type = s_strdup(client_type);
            }
        }
    }
}

/**
 * Get online buddies
 * NB : This function must be called after lwqq_info_get_friends_info()
 * because we stored buddy's status in buddy object which is created in
 * lwqq_info_get_friends_info()
 *
 * @param lc
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_online_buddies(LwqqClient *lc, LwqqErrorCode *err)
{
    char url[512];
    LwqqHttpRequest *req = NULL;
    char *cookies;

    if (!lc) {
        return NULL;
    }

    /* Create a GET request */
    snprintf(url, sizeof(url),
             "%s/channel/get_online_buddies2?clientid=%s&psessionid=%s",
             "http://d.web2.qq.com", lc->clientid, lc->psessionid);
    req = lwqq_http_create_default_request(url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://d.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "utf-8");
    cookies = lwqq_get_cookies(lc);
    if (cookies) {
        req->set_header(req, "Cookie", cookies);
        s_free(cookies);
    }
    return req->do_request_async(req, 0, NULL,get_online_buddies_back,lc);
done:
    lwqq_http_request_free(req);
    return NULL;
}
static int get_online_buddies_back(LwqqHttpRequest* req,void* data)
{
    json_t *json = NULL, *json_tmp;
    int ret;
    LwqqClient* lc = data;
    LwqqErrorCode error;
    LwqqErrorCode* err = &error;

    if (req->http_code != 200) {
        if (err)
            *err = LWQQ_EC_HTTP_ERROR;
        goto done;
    }

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":[{"uin":1100872453,"status":"online","client_type":21},"
     * "{"uin":2726159277,"status":"busy","client_type":1}]}
    */
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        if (err)
            *err = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        goto json_error;
    }

    if (json_tmp->child) {
        json_tmp = json_tmp->child;
        update_online_buddies(lc, json_tmp);
    }

    puts("online buddies complete");
done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;

json_error:
    if (err)
        *err = LWQQ_EC_ERROR;
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;
}
enum CHANGE{CHANGE_BUDDY_MARKNAME,CHANGE_GROUP_MARKNAME,MODIFY_BUDDY_CATEGORY};
LwqqAsyncEvent* lwqq_info_change_buddy_markname(LwqqClient* lc,LwqqBuddy* buddy,const char* alias)
{
    if(!lc||!buddy||!alias) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/change_mark_name2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"tuin=%s&markname=%s&vfwebqq=%s",
            buddy->uin,alias,lc->vfwebqq
            );
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    void** data = s_malloc0(sizeof(void*)*3);
    data[0] = (void*)CHANGE_BUDDY_MARKNAME;
    data[1] = buddy;
    data[2] = s_strdup(alias);
    return req->do_request_async(req,1,post,change_buddy_markname_back,data);
done:
    lwqq_http_request_free(req);
    return NULL;
}

static int change_buddy_markname_back(LwqqHttpRequest* req,void* data)
{
    json_t* root=NULL;
    int errno;
    int ret;

    if(req->http_code!=200){
        errno = LWQQ_EC_HTTP_ERROR;
        goto done;
    }
    puts(req->response);
    ret = json_parse_document(&root,req->response);
    if(ret!=JSON_OK){
        errno = LWQQ_EC_ERROR;
        goto done;
    }
    const char* retcode = json_parse_simple_value(root,"retcode");
    if(retcode==NULL){
        errno = 1;
        goto done;
    }
    errno = atoi(retcode);
    if(errno==0&&data!=NULL){
        void** d = data;
        long type = (long)d[0];
        if(type == CHANGE_BUDDY_MARKNAME){
            LwqqBuddy* buddy = d[1];
            char* alias = d[2];
            if(buddy->markname) s_free(buddy->markname);
            buddy->markname = alias;
        }else if(type == CHANGE_GROUP_MARKNAME){
            LwqqGroup* group = d[1];
            char* alias = d[2];
            if(group->markname) s_free(group->markname);
            group->markname = alias;
        }else if(type == MODIFY_BUDDY_CATEGORY){
            LwqqBuddy* buddy = d[1];
            char* cate_idx = d[2];
            if(buddy->cate_index) s_free(buddy->cate_index);
            buddy->cate_index = cate_idx;
        }
    }
done:
    s_free(data);
    if(root)
        json_free_value(&root);
    lwqq_http_request_free(req);
    return errno;
}


LwqqAsyncEvent* lwqq_info_change_group_markname(LwqqClient* lc,LwqqGroup* group,const char* alias)
{
    if(!lc||!group||!alias) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/update_group_info2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"r={\"gcode\":%s,\"markname\":\"%s\",\"vfwebqq\":\"%s\"}",
            group->code,alias,lc->vfwebqq
            );
    puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    void** data = s_malloc0(sizeof(void*)*3);
    data[0] = (void*)CHANGE_GROUP_MARKNAME;
    data[1] = group;
    data[2] = s_strdup(alias);
    return req->do_request_async(req,1,post,change_buddy_markname_back,data);
done:
    lwqq_http_request_free(req);
    return NULL;
}
LwqqAsyncEvent* lwqq_info_modify_buddy_category(LwqqClient* lc,LwqqBuddy* buddy,const char* new_cate)
{
    if(!lc||!buddy||!new_cate) return NULL;
    int cate_idx = -1;
    LwqqFriendCategory *c;
    LIST_FOREACH(c,&lc->categories,entries){
        if(strcmp(c->name,new_cate)==0){
            cate_idx = c->index;
            break;
        }
    }
    if(cate_idx==-1) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/modify_friend_group","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"tuin=%s&newid=%d&vfwebqq=%s",
            buddy->uin,cate_idx,lc->vfwebqq );
    puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    void** data = s_malloc0(sizeof(void*)*3);
    data[0] = (void*)MODIFY_BUDDY_CATEGORY;
    data[1] = buddy;
    char* cate_index = s_malloc0(11);
    snprintf(cate_index,11,"%d",cate_idx);
    data[2] = cate_index;
    return req->do_request_async(req,1,post,change_buddy_markname_back,data);
done:
    lwqq_http_request_free(req);
    return NULL;
}

LwqqAsyncEvent* lwqq_info_delete_friend(LwqqClient* lc,LwqqBuddy* buddy,LWQQ_DEL_FRIEND_TYPE del_type)
{
    if(!lc||!buddy) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/delete_friend","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"tuin=%s&delType=%d&vfwebqq=%s",
            buddy->uin,del_type,lc->vfwebqq );
    puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    return req->do_request_async(req,1,post,change_buddy_markname_back,NULL);
done:
    lwqq_http_request_free(req);
    return NULL;
}

//account is qqnumber
LwqqAsyncEvent* lwqq_info_allow_added_request(LwqqClient* lc,const char* account)
{
    if(!lc||!account) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/allow_added_request2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"r={\"account\":%s,\"vfwebqq\":\"%s\"}",
            account,lc->vfwebqq );
    puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    return req->do_request_async(req,1,post,change_buddy_markname_back,NULL);
done:
    lwqq_http_request_free(req);
    return NULL;
}

LwqqAsyncEvent* lwqq_info_deny_added_request(LwqqClient* lc,const char* account,const char* reason)
{
    if(!lc||!account) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/deny_added_request2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"r={\"account\":%s,\"vfwebqq\":\"%s\"",
            account,lc->vfwebqq );
    if(reason){
        format_append(post,",\"msg\":\"%s\"",reason);
    }
    format_append(post,"}");
    puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    return req->do_request_async(req,1,post,change_buddy_markname_back,NULL);
done:
    lwqq_http_request_free(req);
    return NULL;
}
LwqqAsyncEvent* lwqq_info_allow_and_add(LwqqClient* lc,const char* account,const char* markname)
{
    if(!lc||!account) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/allow_and_add2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"r={\"account\":%s,\"gid\":0,\"vfwebqq\":\"%s\"",
            account,lc->vfwebqq );
    if(markname){
        format_append(post,",\"mname\":\"%s\"",markname);
    }
    format_append(post,"}");
    puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    return req->do_request_async(req,1,post,change_buddy_markname_back,NULL);
done:
    lwqq_http_request_free(req);
    return NULL;
}
