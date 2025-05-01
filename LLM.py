from dashscope import Generation
from dashscope.api_entities.dashscope_response import Role
from langchain_community.vectorstores import Chroma
from langchain_community.embeddings.huggingface import HuggingFaceEmbeddings
import dashscope

dashscope.api_key="sk-6d3f5ac504e64907ba7a3e7bce73e6e9"

messages = []

model_name = r"text2vec"
model_kwargs = {'device': 'cpu'}
encode_kwargs = {'normalize_embeddings': False}
embeddings = HuggingFaceEmbeddings(
    model_name=model_name,
    model_kwargs=model_kwargs,
    encode_kwargs=encode_kwargs
)
db = Chroma(persist_directory="./chroma/news_test", embedding_function=embeddings)

while True:
    message = input('user:')

    similarDocs = db.similarity_search(message, k=3)
    summary_prompt = "".join([doc.page_content for doc in similarDocs])

    send_message = f"你是一个只能回答助手,请参考{summary_prompt}对{message}的问题进行解答，如果{summary_prompt}中没有提及，请直接回答"
    messages.append({'role': Role.USER, 'content': send_message})
    whole_message = ''
    # 切换模型
    responses = Generation.call(Generation.Models.qwen_max, messages=messages, result_format='message', stream=True, incremental_output=True)
    # responses = Generation.call(Generation.Models.qwen_turbo, messages=messages, result_format='message', stream=True, incremental_output=True)
    print('system:',end='')
    for response in responses:
        whole_message += response.output.choices[0]['message']['content']
        print(response.output.choices[0]['message']['content'], end='')
    print()
    messages.append({'role': 'assistant', 'content': whole_message})

