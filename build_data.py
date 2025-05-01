from langchain_community.vectorstores import Chroma
from langchain_community.embeddings.huggingface import HuggingFaceEmbeddings
import sentence_transformers
from langchain_community.document_loaders import UnstructuredFileLoader,TextLoader
from langchain.text_splitter import RecursiveCharacterTextSplitter


# 导入文本
loader = TextLoader("DoubleHandRobot")
print(2)
data = loader.load()
print(f'documents:{len(data)}')

# 文本切分
text_splitter = RecursiveCharacterTextSplitter(chunk_size=50, chunk_overlap=0)
split_docs = text_splitter.split_documents(data)
print(split_docs)

model_name = r"text2vec"
print(1)
model_kwargs = {'device': 'cpu'}
encode_kwargs = {'normalize_embeddings': False}
embeddings = HuggingFaceEmbeddings(
    model_name=model_name,
    model_kwargs=model_kwargs,
    encode_kwargs=encode_kwargs
)

print(3)


#保存向量数据库部分

# 初始化数据库
db = Chroma.from_documents(split_docs, embeddings,persist_directory="./chroma/jaka_doubleHand")
# 持久化 之后不用初始化
db.persist()
# # 对数据进行加载
# db = Chroma(persist_directory="./chroma/news_test", embedding_function=embeddings)

# # # 寻找四个相似的样本
# # similarDocs = db.similarity_search(question,k=4)


