#include "file_server.h"
#include "os32.h"
#include "sd_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "fileserver";

namespace os32 {

// --- Path helpers ---

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_len - 1; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            int h = hex_digit(src[si + 1]);
            int l = hex_digit(src[si + 2]);
            if (h >= 0 && l >= 0) {
                dst[di++] = (char)((h << 4) | l);
                si += 2;
                continue;
            }
        }
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static bool is_path_safe(const char *path)
{
    if (!path || path[0] != '/') return false;
    if (strstr(path, "..")) return false;
    return true;
}

static bool build_sd_path(char *out, size_t len, const char *rel_path)
{
    if (!is_path_safe(rel_path)) return false;
    snprintf(out, len, "%s%s", SD_MOUNT, rel_path);
    // Remove trailing slash (but keep SD_MOUNT root intact)
    size_t slen = strlen(out);
    size_t mount_len = strlen(SD_MOUNT);
    if (slen > mount_len && out[slen - 1] == '/') {
        out[slen - 1] = '\0';
    }
    return true;
}

static const char* content_type_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, "png") == 0) return "image/png";
    if (strcasecmp(dot, "bmp") == 0) return "image/bmp";
    if (strcasecmp(dot, "gif") == 0) return "image/gif";
    if (strcasecmp(dot, "txt") == 0 || strcasecmp(dot, "log") == 0) return "text/plain";
    if (strcasecmp(dot, "html") == 0 || strcasecmp(dot, "htm") == 0) return "text/html";
    if (strcasecmp(dot, "css") == 0) return "text/css";
    if (strcasecmp(dot, "js") == 0) return "text/javascript";
    if (strcasecmp(dot, "json") == 0) return "application/json";
    return "application/octet-stream";
}

// --- HTML UI ---

static const char INDEX_HTML[] = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>os32 Files</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#282828;color:#ebdbb2;font:14px/1.6 monospace;padding:16px;max-width:800px;margin:0 auto}
h1{color:#fabd2f;margin-bottom:4px;font-size:18px}
.sub{color:#928374;font-size:12px;margin-bottom:12px}
.path{color:#928374;margin-bottom:12px}
.path a{color:#8ec07c}
a{color:#8ec07c;text-decoration:none}
a:hover{text-decoration:underline}
table{width:100%;border-collapse:collapse}
th{text-align:left;color:#928374;border-bottom:1px solid #504945;padding:4px 8px}
th a{color:#928374}
th a:hover{color:#ebdbb2}
td{padding:4px 8px;border-bottom:1px solid #3c3836}
tr:hover{background:#3c3836}
.dir{color:#83a598}
.sz{color:#928374;text-align:right;white-space:nowrap}
.dt{color:#928374;text-align:right;white-space:nowrap}
.del{color:#fb4934;cursor:pointer;font-size:12px;text-align:right}
.del:hover{text-decoration:underline}
.bar{display:flex;gap:8px;margin-bottom:12px;align-items:center;flex-wrap:wrap}
button{background:#504945;color:#ebdbb2;border:1px solid #665c54;padding:4px 12px;cursor:pointer;font:13px monospace;border-radius:2px}
button:hover{background:#665c54}
input[type=file]{font:13px monospace;color:#ebdbb2}
.msg{color:#b8bb26;margin:8px 0;min-height:20px}
.err{color:#fb4934}
</style></head><body>
<h1>os32</h1>
<p class="sub">file server</p>
<div class="path" id="path"></div>
<div class="bar">
<input type="file" id="filein" multiple>
<button onclick="upload()">Upload</button>
<button onclick="newdir()">New Folder</button>
<button onclick="browse(cwd)">Refresh</button>
</div>
<div class="msg" id="msg"></div>
<table><thead><tr><th><a href="#" onclick="setSort('name');return false" id="thName">Name</a></th><th style="text-align:right"><a href="#" onclick="setSort('size');return false" id="thSize">Size</a></th><th style="text-align:right"><a href="#" onclick="setSort('date');return false" id="thDate">Date</a></th><th style="text-align:right"></th></tr></thead>
<tbody id="list"></tbody></table>
<script>
let cwd='/',sortKey='name',sortAsc=true;
function fmt(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';return(b/1048576).toFixed(1)+' MB'}
function fmtDate(t){if(!t)return'-';let d=new Date(t*1000);return d.toLocaleDateString()+' '+d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})}
function sortEntries(entries){entries.sort(function(a,b){if(a.dir!==b.dir)return a.dir?-1:1;if(sortKey==='date'){let r=((a.mtime||0)-(b.mtime||0));return sortAsc?r:-r}if(sortKey==='size'){let r=(a.size||0)-(b.size||0);return sortAsc?r:-r}let r=a.name.toLowerCase().localeCompare(b.name.toLowerCase());return sortAsc?r:-r})}
function setSort(key){if(sortKey===key)sortAsc=!sortAsc;else{sortKey=key;sortAsc=key==='name'}browse(cwd)}
function sortArrow(key){return sortKey===key?(sortAsc?' \u25B2':' \u25BC'):''}
function parent(p){let i=p.lastIndexOf('/',p.length-2);return i<=0?'/':p.substring(0,i+1)}
function show(msg,err){let e=document.getElementById('msg');e.className=err?'msg err':'msg';e.textContent=msg;if(msg)setTimeout(()=>{if(e.textContent===msg)e.textContent=''},3000)}
function renderList(entries,p){
sortEntries(entries);
document.getElementById('thName').textContent='Name'+sortArrow('name');
document.getElementById('thSize').textContent='Size'+sortArrow('size');
document.getElementById('thDate').textContent='Date'+sortArrow('date');
let tb=document.getElementById('list');
tb.textContent='';
if(p!=='/'){let tr=document.createElement('tr');let td=document.createElement('td');td.colSpan=4;
let a=document.createElement('a');a.className='dir';a.href='#';a.textContent='..';a.onclick=function(e){e.preventDefault();browse(parent(p))};
td.appendChild(a);tr.appendChild(td);tb.appendChild(tr)}
for(let e of entries){let tr=document.createElement('tr');
let tdName=document.createElement('td');
let a=document.createElement('a');
let fp=p+(p.endsWith('/')?'':'/')+e.name;
if(e.dir){a.className='dir';a.href='#';a.textContent='/'+e.name;a.onclick=(function(path){return function(ev){ev.preventDefault();browse(path)}})(fp)}
else{a.href='/api/download?path='+encodeURIComponent(fp);a.download=e.name;a.textContent=e.name}
tdName.appendChild(a);tr.appendChild(tdName);
let tdSz=document.createElement('td');tdSz.className='sz';tdSz.textContent=e.dir?'-':fmt(e.size);tr.appendChild(tdSz);
let tdDate=document.createElement('td');tdDate.className='dt';tdDate.textContent=fmtDate(e.mtime);tr.appendChild(tdDate);
let tdDel=document.createElement('td');tdDel.style.textAlign='right';let sp=document.createElement('span');sp.className='del';sp.textContent='delete';
sp.onclick=(function(path){return function(){del(path)}})(fp);
tdDel.appendChild(sp);tr.appendChild(tdDel);tb.appendChild(tr)}}
function renderBreadcrumb(p){
let bc=document.getElementById('path');bc.textContent='';
let a0=document.createElement('a');a0.href='#';a0.textContent='/sdcard';a0.onclick=function(e){e.preventDefault();browse('/')};bc.appendChild(a0);
if(p!=='/'){let parts=p.split('/').filter(Boolean),acc='';
for(let s of parts){acc+='/'+s;bc.appendChild(document.createTextNode(' / '));
let a=document.createElement('a');a.href='#';a.textContent=s;a.onclick=(function(path){return function(e){e.preventDefault();browse(path)}})(acc);bc.appendChild(a)}}}
async function browse(p){
cwd=p;let r;try{r=await fetch('/api/files?path='+encodeURIComponent(p))}catch(e){show('Connection failed',1);return}
if(!r.ok){show('Failed to list directory',1);return}
let d=await r.json();renderBreadcrumb(p);renderList(d.entries,p)}
async function del(path){
if(!confirm('Delete '+path+'?'))return;
let r=await fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path})});
if(r.ok){show('Deleted');browse(cwd)}else show('Delete failed',1)}
async function upload(){
let files=document.getElementById('filein').files;
if(!files.length){show('No file selected',1);return}
for(let f of files){
show('Uploading '+f.name+'...');
let r=await fetch('/api/upload?path='+encodeURIComponent(cwd)+'&name='+encodeURIComponent(f.name),{method:'POST',body:f});
if(!r.ok){show('Upload failed: '+f.name,1);return}}
show('Uploaded '+files.length+' file(s)');document.getElementById('filein').value='';browse(cwd)}
async function newdir(){
let name=prompt('Folder name:');if(!name)return;
let path=cwd+(cwd.endsWith('/')?'':'/')+name;
let r=await fetch('/api/mkdir',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path})});
if(r.ok){show('Created');browse(cwd)}else show('Create failed',1)}
browse('/');
</script></body></html>)rawliteral";

// --- HTTP Handlers ---

struct ServerCtx {
    SdManager *sd;
};

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
    return ESP_OK;
}

static esp_err_t handle_list(httpd_req_t *req)
{
    auto *ctx = static_cast<ServerCtx *>(req->user_ctx);
    if (!ctx->sd->mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not mounted");
        return ESP_FAIL;
    }

    char query[256] = {};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char raw[128] = "/", path_param[128];
    httpd_query_key_value(query, "path", raw, sizeof(raw));
    url_decode(path_param, sizeof(path_param), raw);

    char full_path[256];
    if (!build_sd_path(full_path, sizeof(full_path), path_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory not found");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path_param);
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.') continue;

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", ent->d_name);
        bool is_dir = (ent->d_type == DT_DIR);
        cJSON_AddBoolToObject(obj, "dir", is_dir);

        uint32_t size = 0;
        uint32_t mtime = 0;
        {
            char file_path[512];
            snprintf(file_path, sizeof(file_path), "%.255s/%.255s", full_path, ent->d_name);
            struct stat st;
            if (stat(file_path, &st) == 0) {
                if (!is_dir) size = st.st_size;
                mtime = st.st_mtime;
            }
        }
        cJSON_AddNumberToObject(obj, "size", size);
        cJSON_AddNumberToObject(obj, "mtime", mtime);
        cJSON_AddItemToArray(entries, obj);
    }
    closedir(dir);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_download(httpd_req_t *req)
{
    auto *ctx = static_cast<ServerCtx *>(req->user_ctx);
    if (!ctx->sd->mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not mounted");
        return ESP_FAIL;
    }

    char query[256] = {};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char raw[128] = {}, path_param[128];
    httpd_query_key_value(query, "path", raw, sizeof(raw));
    url_decode(path_param, sizeof(path_param), raw);

    char full_path[256];
    if (!build_sd_path(full_path, sizeof(full_path), path_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    size_t file_size = st.st_size;
    char *file_buf = static_cast<char *>(heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM));
    if (!file_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        heap_caps_free(file_buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    size_t nread = fread(file_buf, 1, file_size, f);
    fclose(f);

    httpd_resp_set_type(req, content_type_for(full_path));

    const char *filename = strrchr(full_path, '/');
    filename = filename ? filename + 1 : full_path;
    char disposition[300];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%.255s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Connection", "close");

    esp_err_t err = httpd_resp_send(req, file_buf, nread);
    heap_caps_free(file_buf);

    // Close the TCP session after download so browser sees FIN promptly
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return err;
}

static esp_err_t handle_upload(httpd_req_t *req)
{
    auto *ctx = static_cast<ServerCtx *>(req->user_ctx);
    if (!ctx->sd->mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not mounted");
        return ESP_FAIL;
    }

    char query[256] = {};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char raw_dir[128] = "/", dir_param[128];
    char raw_name[128] = {}, name_param[128];
    httpd_query_key_value(query, "path", raw_dir, sizeof(raw_dir));
    httpd_query_key_value(query, "name", raw_name, sizeof(raw_name));
    url_decode(dir_param, sizeof(dir_param), raw_dir);
    url_decode(name_param, sizeof(name_param), raw_name);

    if (name_param[0] == '\0' || !is_path_safe(dir_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path or name");
        return ESP_FAIL;
    }

    // Reject filenames with path separators
    if (strchr(name_param, '/') || strstr(name_param, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    char full_dir[256];
    if (!build_sd_path(full_dir, sizeof(full_dir), dir_param)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%.255s/%.255s", full_dir, name_param);

    FILE *f = fopen(full_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create %s: errno=%d (%s)", full_path, errno, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    char buf[1024];
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            fclose(f);
            unlink(full_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        fwrite(buf, 1, received, f);
        remaining -= received;
    }

    fclose(f);
    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", full_path, req->content_len);
    ctx->sd->notify_change();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_delete(httpd_req_t *req)
{
    auto *ctx = static_cast<ServerCtx *>(req->user_ctx);
    if (!ctx->sd->mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not mounted");
        return ESP_FAIL;
    }

    char body[256];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *json = cJSON_Parse(body);
    cJSON *jp = json ? cJSON_GetObjectItem(json, "path") : nullptr;
    if (!jp || !cJSON_IsString(jp)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_FAIL;
    }

    char full_path[256];
    if (!build_sd_path(full_path, sizeof(full_path), jp->valuestring)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }
    cJSON_Delete(json);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    int rc = S_ISDIR(st.st_mode) ? rmdir(full_path) : unlink(full_path);
    if (rc != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted: %s", full_path);
    ctx->sd->notify_change();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_mkdir_req(httpd_req_t *req)
{
    auto *ctx = static_cast<ServerCtx *>(req->user_ctx);
    if (!ctx->sd->mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not mounted");
        return ESP_FAIL;
    }

    char body[256];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *json = cJSON_Parse(body);
    cJSON *jp = json ? cJSON_GetObjectItem(json, "path") : nullptr;
    if (!jp || !cJSON_IsString(jp)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_FAIL;
    }

    char full_path[256];
    if (!build_sd_path(full_path, sizeof(full_path), jp->valuestring)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }
    cJSON_Delete(json);

    if (mkdir(full_path, 0755) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created dir: %s", full_path);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// --- Server lifecycle ---

static ServerCtx s_ctx;

void FileServer::start(SdManager *sd)
{
    if (server_) return;

    s_ctx.sd = sd;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;  // seconds, for large uploads

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server");
        return;
    }

    httpd_uri_t index_uri  = {.uri = "/",             .method = HTTP_GET,  .handler = handle_index,     .user_ctx = &s_ctx};
    httpd_uri_t list_uri   = {.uri = "/api/files",    .method = HTTP_GET,  .handler = handle_list,      .user_ctx = &s_ctx};
    httpd_uri_t dl_uri     = {.uri = "/api/download", .method = HTTP_GET,  .handler = handle_download,  .user_ctx = &s_ctx};
    httpd_uri_t upload_uri = {.uri = "/api/upload",   .method = HTTP_POST, .handler = handle_upload,    .user_ctx = &s_ctx};
    httpd_uri_t delete_uri = {.uri = "/api/delete",   .method = HTTP_POST, .handler = handle_delete,    .user_ctx = &s_ctx};
    httpd_uri_t mkdir_uri  = {.uri = "/api/mkdir",    .method = HTTP_POST, .handler = handle_mkdir_req, .user_ctx = &s_ctx};

    httpd_register_uri_handler(server_, &index_uri);
    httpd_register_uri_handler(server_, &list_uri);
    httpd_register_uri_handler(server_, &dl_uri);
    httpd_register_uri_handler(server_, &upload_uri);
    httpd_register_uri_handler(server_, &delete_uri);
    httpd_register_uri_handler(server_, &mkdir_uri);

    ESP_LOGI(TAG, "File server started on port 8080");
}

void FileServer::stop()
{
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "File server stopped");
    }
}

} // namespace os32
