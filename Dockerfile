FROM 192.168.138.139:30002/library/cpp-backend:latest

RUN apt-get update && apt-get install -y \
    libcurl4-openssl-dev \
    libpq-dev \
    libpqxx-dev \
    redis \
    libldap2-dev \
    python3 \
    libhiredis-dev \
    curl \
    python3-pip \
    ceph-common \
    librados-dev \
    librbd-dev \
    python3-rados \
    libsqlite3-dev \
    golang-go \
    git \
    g++ \
    make \
    cmake \
    libasio-dev \
    libssl-dev \
    nlohmann-json3-dev \
    libopenblas-dev \
    && rm -rf /var/lib/apt/lists/*

# 安装 kubectl
COPY kubectl /usr/local/bin/kubectl
RUN chmod +x /usr/local/bin/kubectl

# 复制 kubeconfig 文件
COPY kubeconfig.yaml /root/.kube/config

# 安装 Helm
COPY helm /usr/local/bin/helm
RUN chmod +x /usr/local/bin/helm

# ========== 安装 Python 依赖 ==========
RUN pip3 install --break-system-packages -i https://pypi.tuna.tsinghua.edu.cn/simple \
    flask \
    pytest \
    allure-pytest \
    pytest-html \
    pyyaml \
    psycopg2-binary \
    rados

COPY webshell/webshell /usr/local/bin/webshell
RUN chmod +x /usr/local/bin/webshell

# ========== 安装 Embedding 服务依赖 ==========
RUN pip3 install --break-system-packages torch==2.2.0 --index-url https://download.pytorch.org/whl/cpu
COPY embedding_requirements.txt /app/embedding_requirements.txt
RUN pip3 install --break-system-packages -r /app/embedding_requirements.txt
COPY embedding_service.py /app/embedding_service.py

# ========== 复制 C++ 后端（宿主机编译好的） ==========
COPY crow-k8s-api /app/crow-k8s-api
RUN chmod +x /app/crow-k8s-api

# 复制其他项目文件
RUN mkdir -p /root/a
COPY sdc-integration-tests /root/sdc-integration-tests
COPY flask-ceph-upload /app/flask-ceph-upload
COPY ceph-config /etc/ceph

EXPOSE 8580 5005 5001 8082 5003

COPY start.sh /app/start.sh
RUN chmod +x /app/start.sh

# 复制 kubeconfig 文件（放在最后，确保不被覆盖）
RUN mkdir -p /root/.kube
COPY kubeconfig.yaml /root/.kube/config

CMD ["/app/start.sh"]
