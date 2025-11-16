# Mini-cloud-storage-








## Compile and Run :

 ### (.) Ubuntu/Linux :
   For ubuntu and linux based terminal make a folder of your convienent name(xyz). Now upload all the three files [`server.c`](./server.c)  [`client.c`](./client.c)  [`Makefile`](Makefile) .

   #### Note:
   
      Save or upload the "Makefile"   as it is.

      
 Do not change or rename . Save as capital 'M' and all things in small like "Makefile" . Do not use any extension behind like .m, .c,.cpp ....
   
 Now  open your folder(xyz) in the terminal window. After run the command "make".    

 
![image.alt](https://github.com/madhav-p-11/Mini-cloud-storage-/blob/main/Screenshot%20from%202025-11-16%2020-13-46.png)

 After running above command run the command "./server 8080 storage" and the message will appear as "Server listening on port 8080, storage: storage"

 ![image.alt](https://github.com/madhav-p-11/Mini-cloud-storage-/blob/main/Screenshot%20from%202025-11-16%2020-13-58.png
 
 
 This will run your server part on 8080 port and will create storage folder to handle files .


 Suppose you are running your server in terminal (T1):
   
 ##### T1->  ./server 8080 storage              
 
 ##here T2-> is just to show that it is terminal it is not part of command##

 Now you have to open another terminal (T2) to run client . You can run multiple clients simultaneously with server in multiple termianls like (T2,T3...) respectively.
 To start client_1  , run the command in T2 terminal as "./client 127.0.0.1 8080" and message will appear as "  OK WELCOME"
  
 ##### T2->  ./client 127.0.0.1 8080

 Likewise you can run multiple client in T3,T4,T4 .....etc.
    
   


   

   
