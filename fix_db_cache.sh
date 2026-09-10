#!/bin/bash

# 备份
cp index.html index.html.before_fix

# 修改 loadImageSelect 函数，从数据库缓存加载
sed -i '/async function loadImageSelect() {/,/^    }/c\
async function loadImageSelect() {\
    const H = "192.168.138.139:30002";\
    const input = document.getElementById("imageSearch");\
    if (!input) return;\
    input.value = "⏳ 加载镜像...";\
    input.disabled = true;\
    try {\
        const r = await fetch("/api/harbor/cached-images");\
        const d = await r.json();\
        window.allImages = [];\
        d.forEach(image => {\
            window.allImages.push({\
                name: image.name,\
                tag: image.tag,\
                full: `${H}/${image.name}:${image.tag}`\
            });\
        });\
        input.value = "";\
        input.placeholder = `🔍 共 ${window.allImages.length} 个镜像，输入关键字搜索...`;\
        if (window.pendingImage) {\
            input.value = window.pendingImage;\
            filterImages();\
            window.pendingImage = null;\
        }\
    } catch (e) {\
        console.error("加载镜像失败:", e);\
        input.placeholder = "加载失败，请刷新页面";\
    } finally {\
        input.disabled = false;\
    }\
}' index.html

# 同时修改 filterImages 函数，确保能正确过滤
sed -i 's/d.forEach(image => {/d.forEach(img => {/g' index.html
sed -i 's/img.name === image.name/img.name === img.name/g' index.html

echo "✅ 已修改为从数据库缓存加载镜像"
