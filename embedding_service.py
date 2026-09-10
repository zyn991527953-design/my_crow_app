"""
Embedding 服务 - 使用 sentence-transformers 提供语义向量
使用本地模型，不需要联网下载
"""
import os
from fastapi import FastAPI
from sentence_transformers import SentenceTransformer
from pydantic import BaseModel
from typing import List
import uvicorn
import logging

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI(title="Embedding Service")

# 使用本地模型路径（从 Dockerfile 复制）
MODEL_PATH = "/app/models/paraphrase-multilingual-MiniLM-L12-v2"
logger.info(f"正在加载本地模型: {MODEL_PATH}...")
model = SentenceTransformer(MODEL_PATH)
logger.info(f"模型加载完成，向量维度: {model.get_sentence_embedding_dimension()}")

class EmbedRequest(BaseModel):
    texts: List[str]

class EmbedResponse(BaseModel):
    embeddings: List[List[float]]
    dimension: int

@app.get("/health")
async def health():
    return {"status": "ok", "model": MODEL_PATH, "dimension": model.get_sentence_embedding_dimension()}

@app.post("/embed", response_model=EmbedResponse)
async def embed(req: EmbedRequest):
    logger.info(f"Embedding {len(req.texts)} texts")
    embeddings = model.encode(req.texts, normalize_embeddings=True)
    return EmbedResponse(
        embeddings=embeddings.tolist(),
        dimension=embeddings.shape[1]
    )

@app.post("/embed_single")
async def embed_single(text: str):
    embedding = model.encode([text], normalize_embeddings=True)
    return {"embedding": embedding.tolist()[0], "dimension": len(embedding[0])}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=5003, log_level="info")
