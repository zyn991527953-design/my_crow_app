package main

import (
    "encoding/json"
    "flag"
    "log"
    "net/http"
    "os"

    "github.com/gorilla/websocket"
    corev1 "k8s.io/api/core/v1"
    "k8s.io/client-go/kubernetes"
    "k8s.io/client-go/kubernetes/scheme"
    "k8s.io/client-go/rest"
    "k8s.io/client-go/tools/clientcmd"
    "k8s.io/client-go/tools/remotecommand"
)

var upgrader = websocket.Upgrader{
    CheckOrigin: func(r *http.Request) bool { return true },
}

type TerminalMessage struct {
    Operation string `json:"operation"`
    Data      string `json:"data"`
}

type WsWrapper struct {
    conn *websocket.Conn
}

func (w *WsWrapper) Read(p []byte) (int, error) {
    _, msg, err := w.conn.ReadMessage()
    if err != nil {
        return 0, err
    }
    var tm TerminalMessage
    if err := json.Unmarshal(msg, &tm); err != nil {
        return 0, err
    }
    if tm.Operation == "stdin" {
        return copy(p, tm.Data), nil
    }
    return 0, nil
}

func (w *WsWrapper) Write(p []byte) (int, error) {
    msg, _ := json.Marshal(TerminalMessage{Operation: "stdout", Data: string(p)})
    err := w.conn.WriteMessage(websocket.TextMessage, msg)
    if err != nil {
        return 0, err
    }
    return len(p), nil
}

func (w *WsWrapper) Next() *remotecommand.TerminalSize {
    return nil
}

func getK8sConfig() (*rest.Config, error) {
    // 优先使用 kubeconfig 文件
    kubeconfig := os.Getenv("KUBECONFIG")
    if kubeconfig == "" {
        kubeconfig = "/root/.kube/config"
    }
    
    // 检查文件是否存在
    if _, err := os.Stat(kubeconfig); err == nil {
        log.Printf("Using kubeconfig: %s", kubeconfig)
        return clientcmd.BuildConfigFromFlags("", kubeconfig)
    }
    
    // 降级使用 in-cluster 配置
    log.Printf("Kubeconfig not found, trying in-cluster config")
    return rest.InClusterConfig()
}

func handleWebSocket(w http.ResponseWriter, r *http.Request) {
    namespace := r.URL.Query().Get("namespace")
    podName := r.URL.Query().Get("pod")
    containerName := r.URL.Query().Get("container")
    
    if namespace == "" || podName == "" {
        log.Printf("Missing parameters: namespace=%s, pod=%s", namespace, podName)
        return
    }
    
    if containerName == "" {
        containerName = "app"
    }
    
    log.Printf("Connecting to: %s/%s/%s", namespace, podName, containerName)
    
    conn, err := upgrader.Upgrade(w, r, nil)
    if err != nil {
        log.Printf("Upgrade error: %v", err)
        return
    }
    defer conn.Close()
    
    config, err := getK8sConfig()
    if err != nil {
        log.Printf("Config error: %v", err)
        conn.WriteMessage(websocket.TextMessage, []byte("K8s config error: "+err.Error()))
        return
    }
    
    clientset, err := kubernetes.NewForConfig(config)
    if err != nil {
        log.Printf("Clientset error: %v", err)
        return
    }
    
    req := clientset.CoreV1().RESTClient().Post().
        Resource("pods").
        Name(podName).
        Namespace(namespace).
        SubResource("exec").
        VersionedParams(&corev1.PodExecOptions{
            Command:   []string{"/bin/sh"},
            Stdin:     true,
            Stdout:    true,
            Stderr:    true,
            TTY:       true,
            Container: containerName,
        }, scheme.ParameterCodec)
    
    executor, err := remotecommand.NewSPDYExecutor(config, "POST", req.URL())
    if err != nil {
        log.Printf("Executor error: %v", err)
        return
    }
    
    wrapper := &WsWrapper{conn: conn}
    err = executor.Stream(remotecommand.StreamOptions{
        Stdin:  wrapper,
        Stdout: wrapper,
        Stderr: wrapper,
        Tty:    true,
    })
    if err != nil {
        log.Printf("Stream error: %v", err)
    }
}

func main() {
    port := flag.String("port", "8082", "port")
    flag.Parse()
    http.HandleFunc("/ws", handleWebSocket)
    log.Printf("Terminal server on :%s", *port)
    log.Fatal(http.ListenAndServe(":"+*port, nil))
}
