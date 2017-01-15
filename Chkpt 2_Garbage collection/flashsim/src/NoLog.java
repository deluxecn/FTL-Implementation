import java.io.*;

public class NoLog {
    public static void main(String[] args) {
        if (args.length != 2) {
          System.out.println("Usage: Java NoLog <FileName> <Operation: delete | comment>");
          return;
        }
        String fileName = args[0];
        String op = args[1];
        String line = null;
        int ind = -1;
        if (op.equals("delete")) {
            try {
                FileReader fr = new FileReader(fileName);
                BufferedReader br = new BufferedReader(fr);
                while ((line = br.readLine()) != null) {
                    if (line.indexOf("std::cout") == -1) {
                        System.out.println(line);
                    } else {
                        while (!((ind = line.indexOf(";")) != -1 && line.substring(ind).trim().equals(";"))) {
                            line = br.readLine();
                        }
                    }
                }
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
        if (op.equals("comment")) {
            try {
                FileReader fr = new FileReader(fileName);
                BufferedReader br = new BufferedReader(fr);
                while ((line = br.readLine()) != null) {
                    if (line.indexOf("std::cout") == -1) {
                        System.out.println(line);
                    } else {
                        System.out.println("//" + line);
                        while (!((ind = line.indexOf(";")) != -1 && line.substring(ind).trim().equals(";"))) {
                            line = br.readLine();
                            System.out.println("//" + line);
                        }
                    }
                }
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
    }
}
