#include <iostream>
#include <string>
#include <tree_sitter/api.h> //core tree-sitter C api
#include <memory>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <set>
#include <filesystem>//scanning folders

#include <hnswlib/hnswlib.h> //include vectorDB
//http and json
#include <httplib.h>
#include <nlohmann/json.hpp>

using namespace std;

namespace fs = filesystem;
using json = nlohmann::json;

//declare c++ parser function that we are linking from our submodule.
//extern 'C' tells the C++ compiler not to mangle the C function name.
extern "C" TSLanguage *tree_sitter_cpp();

//======
//Graph Data Structure
//======
struct FunctionNode {
    size_t id; //unique ID for vectorDB
    std::string name;
    std::string body;
    set<std::string>calls_to;//edges! which functions does this one call
    string filepath;
};

// the actual knowledge graph(adjacency list)
// maps function name to its node data
unordered_map<std::string,FunctionNode>knowledge_graph;
//to look up a function by its vector DB ID
unordered_map<size_t,string>id_to_name;

//read file from hard drive
string read_file(const string& filepath){
    ifstream file(filepath);
    if(!file.is_open()){
        cerr<<"Error: Could not open file "<<filepath<<endl;
        exit(1);
    }
    stringstream buffer;
    buffer<<file.rdbuf();
    return buffer.str();

}

vector<string>get_cpp_files(const string& dir_path){
    vector<string> files;
    for(const auto& entry: fs::recursive_directory_iterator(dir_path)){
        if(entry.is_regular_file()){
            string path_str=entry.path().string(); //full path
            string ext=entry.path().extension().string();

            //skip build and external library folders
            if(path_str.find("external")!=string::npos || path_str.find("build")!=string::npos){
                continue;
            }
            //look for c++
            if(ext==".cpp" || ext==".h" || ext==".hpp" || ext==".c"){
                files.push_back(path_str);
            }
        }
    }
    return files;
}

// //=====
// // MOCK AI EMBEDDING FUNCTION
// //=====
// // in a real app, an LLM gives us a vector of 1536 floats
// // for now,lets generate a fake 16-dimensional vector based on the string length and char
// vector<float>mock_embed(const string& text){
//     vector<float>vec(16,0.0f);
//     for(size_t i=0;i<text.length();i++){
//         vec[i%16]+=(float)text[i]*0.1f;
//     }
//     return vec;
// }

//=====
// REAL AI EMBEDDING(Ollama REST API)
//=====
vector<float> generate_embedding(const string& text){
    httplib::Client cli("localhost",11434);//connect to local Ollama

    //create JSON payload for embedding API
    json payload={
        {"model", "nomic-embed-text"},
        {"prompt", text}
    };

    auto res = cli.Post("/api/embeddings", payload.dump(), "application/json");
    if(res){
        if(res->status==200){
            json response_json = json::parse(res->body);
            return response_json["embedding"].get<vector<float>>();
        }else{
            //log the error
            cerr<<" ->[Warning] Ollama rejected a chunk (Status: "<< res->status << "). Text Length:"<<text.length()<<" chars.\n";
            return {}; //empty vector
        }
    }else{
        cerr<<"Failed to connect to Ollama. Is it running? "<< httplib::to_string(res.error())<<endl;
        return {};
    }
}


struct ParserDeleter{
    void operator()(TSParser* p)const{
        if(p) ts_parser_delete(p);
    }
};

struct TreeDeleter{
    void operator()(TSTree* t)const{
        if(t) ts_tree_delete(t);
    }
};

struct QueryDeleter{
    void operator()(TSQuery* q)const{
        if(q) ts_query_delete(q);
    }
};

struct QueryCursorDeleter{
    void operator()(TSQueryCursor* qc)const{
        if(qc) ts_query_cursor_delete(qc);
    }
};

using ParserPtr = unique_ptr<TSParser, ParserDeleter>;
using TreePtr = unique_ptr<TSTree, TreeDeleter>;
using QueryPtr = unique_ptr<TSQuery, QueryDeleter>;
using QueryCursorPtr= unique_ptr<TSQueryCursor,QueryCursorDeleter>;

using CStringPtr = unique_ptr<char,decltype(&free)>;

//=====
//REAL LLM  GENERATION (Ollama REST API)
//=====

void ask_llm(const string& que, const string& code_chunk, const string& graph_context){
    httplib::Client cli("localhost", 11434);//connect to local Ollama

    //BUilding prompt(injecting our RAG context)
    string prompt="You are an expert Senior C++ Engineer Helping a junior developer understand a codebase.\n\n";
    prompt+= "CONTEXT (Code):\n"+code_chunk+"\n\n";
    prompt+="CONTEXT (Graph Dependencies):\n"+graph_context+"\n\n";
    prompt+="USER QUESTION:\n"+que+"\n\n";
    prompt+="ANSWER CONCISELY AND CLEARLY:";

    json payload={
        {"model","qwen2.5-coder"},
        {"prompt",prompt},
        {"stream",false}
    };

    cout<<"\n[Thinking... Let the AI cook!]\n"<<endl;

    auto res=cli.Post("/api/generate",payload.dump(),"application/json");
    if(res && res->status==200){
        json response_json=json::parse(res->body);
        cout<<"================ AI ANSWER ================\n";
        cout<<response_json["response"].get<string>()<<'\n';
        cout<<"============================================\n";
    }else{
        cout<< httplib::to_string(res.error())<<'\n';
    }
}

int main(int argc, char* argv[]){
    // check if user provided the right arg
    if(argc<3){
        cerr<<"Usage: "<<argv[0]<<" <path_to_directory> \"<your_question>\"\n";
        cerr<<"Example: "<<argv[0]<<" ../test_repo \"How does calculate_and_print word?\"\n";
        return 1;
    }

    string repo_path=argv[1];
    string user_ques=argv[2];

    cout<<"Starting fastgraphrag AI Engine...\n";
    cout<<"Target Directory: "<<repo_path<<'\n';

    //find all c++ files
    vector<string>target_files=get_cpp_files(repo_path);
    if(target_files.empty()){
        cerr<<"No C++ files found in "<<repo_path<<'\n';
        return 1;
    }
    cout<<"Found "<<target_files.size()<<" C++ files to analyze.\n";

    //1. create new parser instance
    ParserPtr parser(ts_parser_new());

    //2. tell the parser to use c++ grammar rules
    ts_parser_set_language(parser.get(), tree_sitter_cpp());

    //=====
    //Extracting data using queries
    //=====

    //OUTER QUERY:

    // 1. query to capture both name(@func.name) and entire definition(@func.body):
    // look inside its declarator

    string query_str= "((function_definition declarator: (function_declarator declarator:(identifier) @func.name)) @func.body)";

    uint32_t error_offset;
    TSQueryError error_type;

    // 2. compile query
    QueryPtr query(
        ts_query_new(
            tree_sitter_cpp(),
            query_str.c_str(),
            query_str.length(),
            &error_offset,
            &error_type
        )
    );
    
    if(error_type != TSQueryErrorNone){
        cerr<<"Query Error at offset: "<<error_offset<<endl;
        return 1;
    }
    //error in returned pointer
    if(!query){
        cerr<<"Query Error at offset: "<<error_offset<<endl;
        return 1;
    }

    //INNER QUERY:
    // find the function calls inside function body
    string call_query_str="(call_expression function: (identifier) @called)";
    //compile query
    QueryPtr call_query(
        ts_query_new(
            tree_sitter_cpp(),
            call_query_str.c_str(),
            call_query_str.length(),
            &error_offset,
            &error_type
        )
    );

    if(error_type != TSQueryErrorNone){
        cerr<<"Query Error at offset: "<<error_offset<<endl;
        return 1;
    }
    //error in returned pointer
    if(!call_query){
        cerr<<"Query Error at offset: "<<error_offset<<endl;
        return 1;
    }

    // 3. cursor to execute the query against our syntax tree
    QueryCursorPtr query_cursor(
        ts_query_cursor_new()
    );
    //cursor for inner search
    QueryCursorPtr call_cursor(
        ts_query_cursor_new()
    );

    //unique ID counter
    size_t curr_id=0;

    cout<<"Parsing Source Code & Generating Embeddings...\n"<<endl;

    for(const auto& filepath: target_files){
        //3. actual file from test repo
        string src_code=read_file(filepath);
        if(src_code.empty())continue;
        cout<<"Successfully loaded: "<< filepath << " ("<< src_code.length()<<"bytes)\n"<<endl;

        //1. parse the string into an AST(Abstract Syntax Tree)
        TreePtr tree(
            ts_parser_parse_string(
                parser.get(),
                nullptr,
                src_code.c_str(),
                src_code.length()
            )
        );

        // 2. get the root node of the tree(top of hierarchy)
        TSNode root_node = ts_tree_root_node(tree.get());

        ts_query_cursor_exec(
            query_cursor.get(),
            query.get(),
            root_node
        );
        TSQueryMatch match;

        // 4. loop through all the matches found in code
        while(ts_query_cursor_next_match(query_cursor.get(), &match)){
            // string curr_func_name="";
            // string curr_func_body="";
            FunctionNode node;
            //assign ID and increment
            node.id=curr_id++;
            node.filepath=filepath;//tag it with its file
            TSNode body_node;

            //loop through the captures in this match
            for(uint32_t i=0;i<match.capture_count;i++){
                TSNode capture_node = match.captures[i].node;

                //start and end byte positions of func_name
                uint32_t start_byte=ts_node_start_byte(capture_node);
                uint32_t end_byte=ts_node_end_byte(capture_node);

                //extracted text
                string extracted_txt=src_code.substr(start_byte,end_byte-start_byte);

                //get the name of capture tag(e.g., "func.name" or "func.body")
                uint32_t length;
                const char *capture_name = ts_query_capture_name_for_id(query.get(), match.captures[i].index, &length);

                string tag(capture_name, length);

                if(tag=="func.name")node.name=extracted_txt;
                if(tag=="func.body"){
                    node.body=extracted_txt;
                    body_node=capture_node; // save the AST node of body so we can search inside it
                }

            }
            //=====
            // Graph Building: Find outgoing edges
            //=====
            //Execute the inner query Only inside the current function's body
            ts_query_cursor_exec(
                call_cursor.get(),
                call_query.get(),
                body_node
            );
            TSQueryMatch call_match;

            while(ts_query_cursor_next_match(call_cursor.get(), & call_match)){
                TSNode call_node= call_match.captures[0].node;
                uint32_t start = ts_node_start_byte(call_node);
                uint32_t end = ts_node_end_byte(call_node);
                string called_func_name= src_code.substr(start,end-start);

                //add edge to our node
                node.calls_to.insert(called_func_name);
            }

            //add the completed node to our knowledge graph
            knowledge_graph[node.name]=node;
            //save the mapping!
            id_to_name[node.id]=node.name;

            // //print perfect "chunk" ready for a vector database
            // cout<<"[Function Name]: "<<curr_func_name<<endl;
            // cout<<"[Code Chunk]:\n"<<curr_func_body<<'\n'<<endl;
            // cout<<"-----------------------------------"<<endl;
        }
    }


    //=====
    //VECTOR DATABASE( 768 DIMENSIONS hnswlib)
    cout<<"Building Vector DB. Inserting "<<knowledge_graph.size()<<" functions...\n";

    int dim=768;//size of our vectors since nomic-embed-text outputs 768 dimensions
    int mx_elements=10000;//max capacity of our database

    //1. init the Math space(inner product is great for cosine similarity)
    hnswlib::InnerProductSpace space(dim);
    //2. init the HNSW index(the database)
    //M=16(number of connections/element) ef_construction=200(search depth during build)
    hnswlib::HierarchicalNSW<float>* vector_db=new hnswlib::HierarchicalNSW<float>(&space, mx_elements,16,200);

    // 3.insert our functions into database
    size_t mx_chars=4000; //limit for embeddings

    for(const auto&pair:knowledge_graph){
        const FunctionNode& node =pair.second;

        if(node.body.length() <= mx_chars){
            //generate Embedding
            //generate the vector from function's code
            vector<float> embedding = generate_embedding(node.body);

            //check if embedding failed(e.g. chunk was too large)
            if(embedding.empty() || embedding.size()!=dim){
                cout<< "Skipped ["<<node.name<<"] due to embedding failure.\n";
                continue;
            }
            //add it to the database(requires pointer to vector array, and id)
            vector_db->addPoint(embedding.data(),node.id);
            cout<<"Inserted ["<<node.name<<"] into Vector DB.\n";
        }
        //if function is Massive, split it into chunks
        else{
            cout<<"Splitting massive function ["<< node.name<<"] into sub-chunks...\n";

            for(size_t i=0;i<node.body.length(); i+=mx_chars){
                //slice a sub-chunk and make sure we don't go out of bounds
                string sub_chunk=node.body.substr(i,min(mx_chars, node.body.length()-i));

                //add context so th AI knows this is just a pirce of larger function
                string chunk_text="Part of function "+ node.name + ":\n" + sub_chunk;

                vector<float>embedding=generate_embedding(chunk_text);

                if(!embedding.empty() && embedding.size()==dim){
                    //we insert this sub-chunk, but still point it to original node.id
                    vector_db->addPoint(embedding.data(), node.id);
                }
            }
            cout<<"Insert ["<<node.name << "] (Sub-chunked) into Vector DB.\n";
        }
    }

    //-----RAG SEARCH & LLM GENERATION----
    cout<<"\nSearching for context related to: \n";
    cout<<user_ques<<'\n';

    //1.convert the user's question into a vector(embed user question)
    vector<float>query_vector=generate_embedding(user_ques);
    
    //2. Search Database
    auto res=vector_db->searchKnn(query_vector.data(),1);

    if(!res.empty()){
        size_t best_match_id=res.top().second;
        string best_match_name=id_to_name[best_match_id];

        cout<<"-> Target Function Found: "<<best_match_name<<" (in "<<knowledge_graph[best_match_name].filepath<<")\n";
        
        //lets use our graph to get extra content
        //---deep context injection---
        string deep_context="";
        for(const auto& edge:knowledge_graph[best_match_name].calls_to){
            //does this connected function exist in our knowledge graph?
            if(knowledge_graph.find(edge)!=knowledge_graph.end()){
                deep_context+="--- Code for " + edge + "() ---\n";
                deep_context+= knowledge_graph[edge].body + "\n\n";
            }else{
                deep_context+= "--- " + edge + "() (External/Library Function) ---\n";
            }
        }
        //3. send to llm
        ask_llm(user_ques,knowledge_graph[best_match_name].body,deep_context);
    }

    //clean up memory
    delete vector_db;

    // //=====
    // //Print the knowledge graph
    // //=====
    // cout<<"=== In-Memory Knowledge Graph ===\n";
    // for(const auto&pair : knowledge_graph){
    //     cout<<"Node: ["<<pair.first<<"]\n";

    //     if(pair.second.calls_to.empty()){
    //         cout<<" Edges: None (Leaf node)\n";
    //     }else{
    //         for(const auto& edge: pair.second.calls_to){
    //             cout<<" --[CALLS]--> Node: ["<<edge<<"]\n";
    //         }
    //     }
    //     cout<<"------------------------------\n";
    // }

    // //6. convert the tree to readable string format(S-expression)
    // CStringPtr tree_string(
    //     ts_node_string(root_node),
    //     &free
    // );

    // cout<<"=== Abstract Syntax Tree ==="<<endl;
    // cout<<"Analyzing Code: int add(int a,int b){return a+b;}\n"<<tree_string.get()<<endl;

    return 0;
}