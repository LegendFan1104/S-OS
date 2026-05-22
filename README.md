## S-OS队

T2026104869910625

## 团队成员

| 姓名   | 专业   | 学校   |
| ---- | ---- | ---- |
|  包一帆 |   计算机科学与技术   |   武汉大学   |
|  庄廷钊 |   计算机科学与技术   |   武汉大学   |
|  李嘉乐 |   计算机科学与技术   |   武汉大学   |

## 运行方法

1. 拉取docker官方测评镜像
   ```bash
   docker run -it --name sos1 -v "$(pwd)":/workspace -w /workspace zhouzhouyi/os-contest:20260510 bash
   docker start sos1
   docker exec -it sos1 bash
   ```

2. 容器内编译
   ```bash
   make clean
   make all
   ```
   编译后生成kernel-la、kernel-rv内核文件，disk.img、disk-la.img为磁盘文件