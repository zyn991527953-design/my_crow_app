// vector_rag.hpp
#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cstring>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <memory>

using json = nlohmann::json;

// ============ WriteCallback ============
inline size_t RagWriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

// ============ 简单向量 ============
struct Vector {
    std::vector<float> data;
    int dim = 0;

    Vector() = default;
    Vector(const std::vector<float>& d) : data(d), dim((int)d.size()) {}
    Vector(std::vector<float>&& d) : data(std::move(d)), dim((int)data.size()) {}

    float dot(const Vector& other) const {
        float sum = 0;
        size_t n = std::min(data.size(), other.data.size());
        for (size_t i = 0; i < n; i++) sum += data[i] * other.data[i];
        return sum;
    }

    float norm() const {
        float sum = 0;
        for (float v : data) sum += v * v;
        return std::sqrt(sum);
    }

    void normalize() {
        float n = norm();
        if (n > 0) {
            for (float& v : data) v /= n;
        }
    }

    float cosine_similarity(const Vector& other) const {
        float n1 = norm(), n2 = other.norm();
        if (n1 <= 0 || n2 <= 0) return 0;
        return dot(other) / (n1 * n2);
    }
};

// ============ 文档 ============
struct Document {
    std::string id;
    std::string text;
    std::unordered_map<std::string, std::string> metadata;
    Vector embedding;

    Document() = default;
    Document(const std::string& t, const Vector& v) : text(t), embedding(v) {}
};

// ============ 搜索结果 ============
struct SearchResult {
    std::string id;
    std::string text;
    std::unordered_map<std::string, std::string> metadata;
    float score;
};

// ============ 轻量级向量存储 ============
class SimpleVectorStore {
public:
    void add(const std::string& text, const Vector& embedding,
             const std::unordered_map<std::string, std::string>& meta = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        Document doc;
        doc.id = "doc_" + std::to_string(docs_.size());
        doc.text = text;
        doc.embedding = embedding;
        doc.metadata = meta;
        docs_.push_back(std::move(doc));
    }

    std::vector<SearchResult> search(const Vector& query, int top_k = 5) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (docs_.empty()) return {};

        std::vector<std::pair<float, size_t>> scores;
        scores.reserve(docs_.size());

        for (size_t i = 0; i < docs_.size(); i++) {
            float sim = query.cosine_similarity(docs_[i].embedding);
            scores.emplace_back(sim, i);
        }

        std::partial_sort(scores.begin(),
                         scores.begin() + std::min(top_k, (int)scores.size()),
                         scores.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });

        std::vector<SearchResult> results;
        int count = std::min(top_k, (int)scores.size());
        for (int i = 0; i < count; i++) {
            SearchResult r;
            r.id = docs_[scores[i].second].id;
            r.text = docs_[scores[i].second].text;
            r.metadata = docs_[scores[i].second].metadata;
            r.score = scores[i].first;
            results.push_back(r);
        }
        return results;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return docs_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        docs_.clear();
    }

    std::vector<Document> get_all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return docs_;
    }

    bool remove_by_id(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(docs_.begin(), docs_.end(),
            [&id](const Document& doc) { return doc.id == id; });
        if (it != docs_.end()) {
            docs_.erase(it);
            return true;
        }
        return false;
    }

    void clear_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        docs_.clear();
    }

private:
    std::vector<Document> docs_;
    mutable std::mutex mutex_;
};

// ============ HTTP Embedding 客户端 ============
class EmbeddingClient {
public:
    EmbeddingClient(const std::string& url = "http://localhost:5003/embed")
        : url_(url) {}

    Vector embed(const std::string& text) {
        auto results = embed_batch({text});
        if (results.empty()) {
            return Vector(std::vector<float>(384, 0.0f));
        }
        return results[0];
    }

    std::vector<Vector> embed_batch(const std::vector<std::string>& texts) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            return {};
        }

        json body;
        body["texts"] = texts;
        std::string post_data = body.dump();
        std::string response;

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_data.size());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, RagWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            std::cerr << "[ERROR] Embedding 服务请求失败: " << curl_easy_strerror(res) << std::endl;
            return {};
        }

        try {
            auto resp = json::parse(response);
            if (resp.contains("embeddings")) {
                std::vector<Vector> results;
                for (const auto& emb : resp["embeddings"]) {
                    std::vector<float> vec = emb.get<std::vector<float>>();
                    results.emplace_back(std::move(vec));
                }
                return results;
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] 解析 Embedding 响应失败: " << e.what() << std::endl;
        }

        return {};
    }

private:
    std::string url_;
};

// ============ RAG 引擎 ============
class SimpleRAGEngine {
public:
    SimpleRAGEngine(const std::string& llm_url = "",
                    const std::string& embed_url = "http://localhost:5003/embed")
        : llm_url_(llm_url.empty() ? "http://192.168.138.140:3000/api/chat/completions" : llm_url),
          embed_client_(embed_url) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~SimpleRAGEngine() {
        curl_global_cleanup();
    }

    // 添加知识
    void add_knowledge(const std::string& text,
                       const std::unordered_map<std::string, std::string>& meta = {}) {
        Vector emb = embed_client_.embed(text);
        store_.add(text, emb, meta);
    }

    void add_knowledge_batch(const std::vector<std::string>& texts,
                             const std::vector<std::unordered_map<std::string, std::string>>& metadatas = {}) {
        auto embeddings = embed_client_.embed_batch(texts);
        for (size_t i = 0; i < texts.size() && i < embeddings.size(); i++) {
            auto meta = (i < metadatas.size()) ? metadatas[i] : std::unordered_map<std::string, std::string>();
            store_.add(texts[i], embeddings[i], meta);
        }
    }

    // 初始化 K8s 知识库
    void init_k8s_knowledge() {
        std::vector<std::string> knowledge = {
            // Pod 相关
            "Pod NotReady 状态排查：1.检查节点资源(CPU/内存/磁盘) 2.检查网络插件(Calico/Flannel) 3.检查容器运行时(containerd/docker) 4.检查镜像拉取 5.查看 Pod 事件和日志",
            "Pod 启动失败常见原因：1.镜像不存在或拉取失败 2.资源不足(CPU/内存) 3.挂载卷不存在 4.配置错误 5.健康检查失败",
            "Pod 重启循环排查：1.查看容器日志 2.检查内存限制 3.检查健康检查配置 4.检查是否有 panic 或 OOM",
            // 节点相关
            "NodeNotReady 常见原因：1.节点宕机或网络不通 2.Kubelet 服务停止 3.磁盘空间不足(>85%) 4.内存不足导致系统 OOM 5.容器运行时服务异常",
            "节点资源不足处理：1.清理未使用的镜像和容器 2.调整 Pod 资源请求 3.添加新节点 4.驱逐非关键 Pod",
            // OOM 相关
            "OOM 问题排查流程：1.kubectl top nodes 查看节点内存 2.kubectl top pods -n <ns> 查看 Pod 内存 3.检查事件: kubectl get events --field-selector reason=OOMKilling 4.调整内存限制 5.优化应用内存使用",
            "OOM 预防建议：1.设置合理的 memory limit 2.使用 quality-of-service (Guaranteed/Burstable) 3.监控内存使用 4.水平扩展减少单 Pod 压力",
            // 镜像相关
            "镜像拉取失败解决：1.检查镜像仓库地址是否正确 2.检查 registry 认证凭据(secret) 3.检查网络连通性 4.使用 imagePullPolicy: IfNotPresent 5.检查节点磁盘空间 6.使用私有 registry 时确认 secret 已配置",
            // Deployment 相关
            "Deployment 滚动更新：kubectl rollout status deployment/<name> -n <namespace> 查看状态，kubectl rollout undo deployment/<name> -n <namespace> 回滚",
            "Deployment 更新策略：1.RollingUpdate: 逐步替换 2.Recreate: 先删后建 3.设置 maxSurge 和 maxUnavailable 控制速度",
            // Harbor 相关
            "Harbor 镜像管理功能：1.创建项目 (public/private) 2.上传镜像 3.下载镜像 4.标签管理 5.镜像复制 6.漏洞扫描",
            "Harbor API 常用接口：GET /api/v2.0/projects 获取项目列表，POST /api/v2.0/projects 创建项目，GET /api/v2.0/projects/{project}/repositories 获取镜像列表",
            // 网络相关
            "K8s 网络问题排查：1.检查 CNI 插件状态 2.检查 Service/Endpoint 3.检查 NetworkPolicy 4.使用 kubectl exec 测试连通性 5.检查 DNS 解析",
            "Service 无法访问排查：1.检查 Service 类型和端口 2.检查 Endpoints 是否关联 Pod 3.检查网络策略 4.检查节点防火墙 5.检查 ingress 配置",
            // 存储相关
            "PVC 状态 Pending 解决：1.检查 StorageClass 是否存在 2.检查 PV 是否充足 3.检查 PVC 配额 4.检查节点存储驱动",
            "持久化存储问题排查：1.检查 PV/PVC 绑定状态 2.检查存储后端状态 3.检查 Pod 挂载权限 4.检查存储容量",
            // 监控告警
            "K8s 监控常用命令：kubectl top nodes, kubectl top pods -n <ns>, kubectl describe node <name>, kubectl describe pod <name>",
            "集群告警处理：1.查看 Prometheus 告警 2.检查 Grafana 面板 3.查看事件记录 4.分析资源趋势 5.执行扩容或优化",
            // 安全相关
            "K8s 安全最佳实践：1.启用 RBAC 2.使用 ServiceAccount 3.限制容器权限 (securityContext) 4.使用 NetworkPolicy 5.启用 PodSecurityPolicy/Admission"
        };

        std::vector<std::unordered_map<std::string, std::string>> metadatas;
        for (const auto& k : knowledge) {
            metadatas.push_back({{"source", "builtin"}, {"type", "k8s_knowledge"}});
        }

        add_knowledge_batch(knowledge, metadatas);
        std::cout << "[INFO] K8s 知识库初始化完成，共 " << store_.size() << " 条知识" << std::endl;
    }

    // 搜索
    std::vector<SearchResult> search(const std::string& query, int top_k = 5) {
        Vector q = embed_client_.embed(query);
        return store_.search(q, top_k);
    }

    // RAG 问答
    std::string ask(const std::string& query, int top_k = 3) {
        auto results = search(query, top_k);

        if (results.empty()) {
            return "抱歉，没有找到相关信息。请先初始化知识库。";
        }

        std::string context;
        for (size_t i = 0; i < results.size(); i++) {
            context += "[来源" + std::to_string(i+1) + "] " + results[i].text + "\n";
        }

        return call_llm(query, context);
    }

    size_t knowledge_size() const {
        return store_.size();
    }

    std::vector<Document> get_all_knowledge() const {
        return store_.get_all();
    }

    bool delete_knowledge(const std::string& id) {
        return store_.remove_by_id(id);
    }

    void clear_all_knowledge() {
        store_.clear_all();
    }

private:
    EmbeddingClient embed_client_;
    SimpleVectorStore store_;
    std::string llm_url_;

    std::string call_llm(const std::string& query, const std::string& context) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            return "基于知识库检索结果：\n" + context;
        }

        json body;
        body["model"] = "gpt-3.5-turbo";
        body["messages"] = json::array();
        body["messages"].push_back({
            {"role", "system"},
            {"content", "你是K8s运维专家，根据知识库回答用户问题。回答简洁专业。如果信息不足请告知。"}
        });
        body["messages"].push_back({
            {"role", "user"},
            {"content", "知识库：\n" + context + "\n\n用户问题：" + query}
        });
        body["stream"] = false;

        std::string post_data = body.dump();
        std::string response;

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, llm_url_.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_data.size());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, RagWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            return "基于知识库检索结果：\n" + context;
        }

        try {
            auto resp = json::parse(response);
            if (resp.contains("choices") && !resp["choices"].empty()) {
                auto msg = resp["choices"][0]["message"];
                if (msg.contains("content")) {
                    return msg["content"].get<std::string>();
                }
            }
        } catch (...) {}

        return "基于知识库检索结果：\n" + context;
    }
};
