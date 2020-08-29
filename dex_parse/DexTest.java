public class DexTest {
	String name="wsm";
	int age = 13;

	public static void main(String args[]) {
		Hello hello = new Hello();
		hello.sayHello("123", 88);
	}

}

class Hello {
	void sayHello(String name, int age) {
		System.out.println("my name is " + name + ", age = " + age);
	}
}
